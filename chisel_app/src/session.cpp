#include "chisel_app/session.hpp"
#include "chisel_app/output.hpp"
#include "chisel_app/paths.hpp"

#include "event_bus.hpp"
#include "events.hpp"
#include "logger.hpp"
#include "mime_detector.hpp"
#include "options.hpp"
#include "processor_executor.hpp"
#include "processor_registry.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <deque>
#include <mutex>
#include <random>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#ifdef _WIN32
#define NOGDI
#define NOUSER
#include <windows.h>
#endif

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace chisel_app {

    std::string_view to_string(const Status status) {
        switch (status) {
            case Status::Pending: return "pending";
            case Status::Processing: return "processing";
            case Status::NoGain: return "noGain";
            case Status::Skipped: return "skipped";
            case Status::Completed: return "completed";
            case Status::Simulated: return "simulated";
            case Status::Error: return "error";
            case Status::Stopped: return "stopped";
        }
        return "?";
    }

    std::string_view to_string(const SkipNote note) {
        switch (note) {
            case SkipNote::None: return "";
            case SkipNote::ReadOnly: return "Read-only";
            case SkipNote::ChangedDuringRun: return "Changed during the run";
        }
        return "";
    }

    std::string_view to_string(const StartError error) {
        switch (error) {
            case StartError::None: return "none";
            case StartError::AlreadyRunning: return "a run is already going";
            case StartError::NothingPending: return "nothing is waiting to be processed";
            case StartError::NoOutputFolder: return "no output folder is set";
            case StartError::OutputFolderUnavailable: return "the output folder can't be created";
            case StartError::OutputFolderIsInputFolder: return "the output folder is the folder the files come from";
        }
        return "?";
    }

    // SkipKind is chisel::SkipReason shifted by one, None first
    static_assert(static_cast<int>(SkipKind::Junk) == static_cast<int>(chisel::SkipReason::Junk) + 1);
    static_assert(static_cast<int>(SkipKind::ChangedDuringRun) == static_cast<int>(chisel::SkipReason::ChangedDuringRun) + 1);

    namespace {
        using Key = fs::path::string_type;

        std::string format_bytes(const std::uint64_t bytes) {
            const char* units[] = {"bytes", "KB", "MB", "GB", "TB"};
            double value = static_cast<double>(bytes);
            int unit = 0;
            while (value >= 1000.0 && unit < 4) {
                value /= 1000.0;
                ++unit;
            }
            char text[32];
            if (unit == 0) std::snprintf(text, sizeof text, "%llu bytes", static_cast<unsigned long long>(bytes));
            else std::snprintf(text, sizeof text, "%.1f %s", value, units[unit]);
            return text;
        }

        std::string make_run_id() {
            const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
            std::tm tm{};
#ifdef _WIN32
            localtime_s(&tm, &now);
#else
            localtime_r(&now, &tm);
#endif
            char text[48];
            std::snprintf(text, sizeof text, "%04d%02d%02d-%02d%02d%02d-%04x", tm.tm_year + 1900, tm.tm_mon + 1,
                          tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                          static_cast<unsigned>(std::random_device{}() & 0xffff));
            return text;
        }

        bool is_hidden(const fs::directory_entry& entry) {
            const auto name = entry.path().filename().native();
            if (!name.empty() && name.front() == '.') return true;
#ifdef _WIN32
            const DWORD attributes = GetFileAttributesW(entry.path().c_str());
            return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_HIDDEN);
#else
            return false;
#endif
        }
    } // namespace

    struct Session::Impl {
        struct Node {
            Row row;
            std::vector<std::string> logs;
            std::vector<std::uint64_t> children;
            bool in_run = false;     // queued files of the running run
            fs::path import_root;    // queued files: the file or folder they were added from
        };

        struct RootInfo {
            std::uint64_t id = 0;
            std::optional<FileStamp> stamp; // taken before chisel reads it, for the modes chisel doesn't guard
        };

        struct Group {
            std::vector<fs::path> inputs;
            std::vector<fs::path> layout_roots;
            fs::path output_dir;
            fs::path target_folder; // OutputFolder: where the smaller files go, the temp folder being inside it
            bool temp_output = false;
        };

        struct Plan {
            std::vector<Group> groups;
            std::vector<fs::path> read_only;
            chisel::ProcessingOptions options;
            unsigned threads = 1;
        };

        // forwards chisel's log lines to the run, only while it lasts: chisel's Logger is process-wide
        class LogSink final : public chisel::ILogSink {
        public:
            explicit LogSink(Impl* impl) : impl_(impl) {}
            void log(const chisel::LogLevel level, const std::string_view message, const std::string_view tag) override {
                // debug lines come per file and would flood the ui (as in ChiselSwift)
                if (level <= chisel::LogLevel::Debug) return;
                impl_->push_log(static_cast<int>(level), std::string(tag), std::string(message));
                impl_->changed();
            }

        private:
            Impl* impl_;
        };

        struct ScopedSink {
            explicit ScopedSink(Impl* impl) {
                auto owned = std::make_unique<LogSink>(impl);
                sink = owned.get();
                chisel::Logger::add_sink(std::move(owned));
            }
            ~ScopedSink() { chisel::Logger::remove_sink(sink); }
            ScopedSink(const ScopedSink&) = delete;
            ScopedSink& operator=(const ScopedSink&) = delete;
            chisel::ILogSink* sink = nullptr;
        };

        explicit Impl(StatsStore* store) : registry(std::make_unique<chisel::ProcessorRegistry>()), stats_store(store) {}

        std::unique_ptr<chisel::ProcessorRegistry> registry;
        StatsStore* stats_store = nullptr;

        mutable std::mutex mtx;
        std::unordered_map<std::uint64_t, Node> nodes;
        std::vector<std::uint64_t> roots;
        std::unordered_map<Key, std::uint64_t> root_index;
        std::unordered_map<Key, std::uint64_t> run_index;
        std::vector<fs::path> unsupported;
        std::unordered_set<Key> unsupported_set;
        std::uint64_t next_id = 1;
        std::uint64_t version = 0;
        std::unordered_map<Key, Clock::time_point> start_times;
        std::unordered_map<Key, Clock::time_point> analyze_times;
        std::vector<StatRecord> run_records;
        std::vector<StatRecord> last_records;
        std::string run_id;
        bool running = false;
        bool stopping = false;
        bool dry_run = false;
        bool hide_unsupported = true;
        OutputMode mode = OutputMode::KeepOriginal;
        bool ran = false;
        Clock::time_point run_start{};
        Clock::time_point run_end{};
        std::uint64_t unmatched = 0;
        std::vector<std::uint64_t> run_ids; // queued files of the current or last run
        Phase phase = Phase::Idle;
        unsigned threads = 0;

        // written by start() before the run's thread exists, then only read until the run ends
        std::unordered_map<Key, RootInfo> run_roots;

        mutable std::mutex log_mtx;
        std::deque<LogLine> logs;
        std::uint64_t next_log_seq = 1;
        static constexpr std::size_t kMaxLogs = 20000;

        std::mutex exec_mtx;
        chisel::ProcessorExecutor* executor = nullptr;
        std::atomic<bool> stop_requested{false};
        std::thread worker;

        std::mutex callback_mtx;
        std::function<void()> on_changed;
        mutable std::atomic<bool> notify_pending{false};

        // --- notifications and logs

        void changed() {
            if (notify_pending.exchange(true)) return;
            std::function<void()> callback;
            {
                std::lock_guard lock(callback_mtx);
                callback = on_changed;
            }
            if (callback) callback();
        }

        void push_log(const int level, std::string tag, std::string text) {
            std::lock_guard lock(log_mtx);
            logs.push_back({next_log_seq++, level, std::move(tag), std::move(text)});
            if (logs.size() > kMaxLogs) logs.pop_front();
        }

        void global(std::string text) {
            push_log(static_cast<int>(chisel::LogLevel::Info), "chisel_app", std::move(text));
        }

        // --- tree, all under mtx

        struct Target {
            Node* node = nullptr;   // the row the event names
            Node* parent = nullptr; // its container's row
            bool root = false;
            bool found = false;
        };

        // the row chisel names, else the row of the container it says the file came from
        Target resolve(const fs::path& path, const std::optional<fs::path>& parent_hint) {
            Target t;
            if (const auto it = run_index.find(path.native()); it != run_index.end()) {
                t.node = &nodes.at(it->second);
                t.root = t.node->row.parent == 0;
                if (!t.root) t.parent = &nodes.at(t.node->row.parent);
                t.found = true;
                return t;
            }
            if (parent_hint) {
                if (const auto it = run_index.find(parent_hint->native()); it != run_index.end()) {
                    t.parent = &nodes.at(it->second);
                    t.found = true;
                }
            }
            return t;
        }

        Node* create_child(Node& parent, const fs::path& path) {
            const auto id = next_id++;
            Node& node = nodes[id];
            node.row.id = id;
            node.row.parent = parent.row.id;
            node.row.path = path;
            node.row.name = to_utf8(path.filename());
            node.row.extension = lower_extension(path);
            std::error_code ec;
            const auto size = fs::file_size(path, ec);
            node.row.size = ec ? 0 : size;
            parent.children.push_back(id);
            run_index[path.native()] = id;
            return &node;
        }

        // the event's row, created under its container when it has none yet
        Node* child_of(Target& t, const fs::path& path) {
            if (t.node) return t.node;
            if (!t.parent) return nullptr;
            t.node = create_child(*t.parent, path);
            return t.node;
        }

        void remove_subtree(const std::uint64_t id) {
            const auto it = nodes.find(id);
            if (it == nodes.end()) return;
            for (const auto child : std::vector(it->second.children)) remove_subtree(child);
            Node& node = nodes.at(id);
            if (const auto ri = run_index.find(node.row.path.native()); ri != run_index.end() && ri->second == id) {
                run_index.erase(ri);
            }
            if (node.row.parent != 0) {
                auto& siblings = nodes.at(node.row.parent).children;
                std::erase(siblings, id);
            }
            nodes.erase(id);
        }

        template <typename Rule>
        void mark_contents(Node& node, const Rule& rule) {
            for (const auto id : node.children) {
                Node& child = nodes.at(id);
                if (const auto change = rule(child.row.status)) {
                    child.row.status = change->first;
                    if (change->first == Status::Error) child.row.error = change->second.first;
                    child.row.size_after.reset();
                    child.logs.push_back(change->second.second);
                }
                mark_contents(child, rule);
            }
        }

        // a container's final result settles everything inside it, whose results only reach disk through it
        void settle_contents(Node& container) {
            using Change = std::optional<std::pair<Status, std::pair<std::string, std::string>>>;
            const auto& name = container.row.name;
            switch (container.row.status) {
                case Status::Error:
                    mark_contents(container, [&](const Status s) -> Change {
                        if (s == Status::Error) return std::nullopt;
                        return std::pair{Status::Error, std::pair{"Container " + name + " failed",
                                         "ERROR: CONTAINER " + name + " FAILED, NOTHING INSIDE IT WAS SAVED"}};
                    });
                    break;
                case Status::NoGain:
                    mark_contents(container, [&](const Status s) -> Change {
                        if (s != Status::Completed && s != Status::Simulated) return std::nullopt;
                        return std::pair{Status::Skipped, std::pair{std::string{}, "DISCARDED: CONTAINER " + name + " DID NOT GET SMALLER"}};
                    });
                    break;
                case Status::Skipped:
                    mark_contents(container, [&](const Status s) -> Change {
                        if (s != Status::Completed && s != Status::Simulated) return std::nullopt;
                        return std::pair{Status::Skipped, std::pair{std::string{}, "DISCARDED: CONTAINER " + name + " WAS LEFT AS IT WAS"}};
                    });
                    break;
                case Status::Stopped:
                    mark_contents(container, [&](const Status s) -> Change {
                        if (s != Status::Completed && s != Status::NoGain && s != Status::Simulated) return std::nullopt;
                        return std::pair{Status::Stopped, std::pair{std::string{}, "DISCARDED: CONTAINER " + name + " WAS NOT REBUILT"}};
                    });
                    break;
                default:
                    break;
            }
        }

        void mark_remaining_stopped(Node& node) {
            if (node.row.status == Status::Pending) {
                node.row.status = Status::Stopped;
                node.logs.emplace_back("PROCESS ABORTED BY USER");
                global("[" + node.row.name + "] PROCESS ABORTED BY USER");
            } else if (node.row.status == Status::Processing) {
                node.row.status = Status::Stopped;
                node.logs.emplace_back("PROCESS INTERRUPTED BY USER");
                global("[" + node.row.name + "] PROCESS INTERRUPTED BY USER");
            }
            for (const auto id : node.children) mark_remaining_stopped(nodes.at(id));
        }

        // --- event handlers (ChiselSwift's ChiselViewModel.startProcessing), called on chisel's threads

        void on_analyze_start(const fs::path& path) {
            {
                std::lock_guard lock(mtx);
                const auto it = run_index.find(path.native());
                // nested containers get their row once their analysis completes
                if (it == run_index.end()) return;
                Node& node = nodes.at(it->second);
                node.row.status = Status::Processing;
                if (node.row.parent == 0) {
                    analyze_times[path.native()] = Clock::now();
                    node.logs.emplace_back("ANALYZING CONTAINER");
                    global("ANALYZING CONTAINER: " + node.row.name);
                } else {
                    node.logs.emplace_back("[CHILD] ANALYZING CONTAINER");
                }
                ++version;
            }
            changed();
        }

        void on_analyze_complete(const fs::path& path, const std::optional<fs::path>& parent, const bool extracted,
                                 const std::size_t num_children) {
            if (!extracted) return;
            {
                std::lock_guard lock(mtx);
                auto t = resolve(path, parent);
                const auto msg = "EXTRACTION COMPLETE: FOUND " + std::to_string(num_children) + " FILES";
                if (t.root) {
                    t.node->row.container = true;
                    t.node->logs.push_back(msg);
                    global(t.node->row.name + " - " + msg);
                } else if (Node* child = child_of(t, path)) {
                    child->row.container = true;
                    child->row.status = Status::Processing;
                    child->logs.push_back("[CHILD] " + msg);
                } else {
                    ++unmatched;
                    return;
                }
                ++version;
            }
            changed();
        }

        void on_start(const fs::path& path, const std::optional<fs::path>& parent) {
            {
                std::lock_guard lock(mtx);
                if (phase == Phase::Analyzing) phase = Phase::Recompressing;
                start_times[path.native()] = Clock::now();
                auto t = resolve(path, parent);
                if (t.root) {
                    t.node->row.status = Status::Processing;
                    t.node->logs.emplace_back("STARTED PROCESSING");
                    global("STARTED PROCESSING: " + t.node->row.name);
                } else if (Node* child = child_of(t, path)) {
                    child->row.status = Status::Processing;
                    child->logs.emplace_back("STARTED EXTRACTED FILE");
                    if (t.parent->row.status != Status::Processing) t.parent->row.status = Status::Processing;
                    const auto msg = "[CHILD] STARTED: " + to_utf8(path.filename());
                    t.parent->logs.push_back(msg);
                    global(msg);
                } else {
                    ++unmatched;
                    return;
                }
                ++version;
            }
            changed();
        }

        struct Placement {
            fs::path result;
            std::string error;
        };

        void on_finish(const fs::path& path, const std::optional<fs::path>& parent, const std::uint64_t before,
                       const std::uint64_t after, const bool is_container_self, const Placement& placement) {
            {
                std::lock_guard lock(mtx);
                auto t = resolve(path, parent);
                if (!t.found) {
                    ++unmatched;
                    return;
                }
                const auto target_name = to_utf8(path.filename());
                const bool gain = before > after;

                // a mixed container's own stream: Phase 3 still rebuilds it on this result, so it isn't final
                if (is_container_self) {
                    const auto msg = gain
                        ? "COMPRESSED CONTAINER STREAM: " + target_name + " (finalizing embedded content...)"
                        : "NO GAIN ON CONTAINER STREAM: " + target_name + " (finalizing embedded content...)";
                    if (t.root) t.node->logs.push_back(msg);
                    else if (Node* child = child_of(t, path)) child->logs.push_back(msg);
                    global(msg);
                    ++version;
                } else if (t.root) {
                    Node& node = *t.node;
                    const bool failed = gain && !dry_run && !placement.error.empty();
                    std::string msg = gain ? "SUCCESSFULLY COMPRESSED: " + target_name + " (saved " + format_bytes(before - after) + ")"
                                           : "NO GAIN: " + target_name;
                    if (failed) msg = "ERROR SAVING " + target_name + ": " + placement.error;
                    else if (gain && dry_run) msg = "SIMULATED COMPRESSION: " + target_name + " (would save " + format_bytes(before - after) + ")";
                    node.row.size_after = after;
                    node.row.status = failed ? Status::Error : gain ? (dry_run ? Status::Simulated : Status::Completed) : Status::NoGain;
                    node.row.error = failed ? "Save failed: " + placement.error : std::string{};
                    if (node.row.status == Status::Completed) node.row.result = placement.result;
                    node.logs.push_back(msg);
                    global(msg);
                    settle_contents(node);
                    if (gain && !failed && !dry_run) record_stat(node, path, before, after);
                    ++version;
                } else if (Node* child = child_of(t, path)) {
                    std::string msg = gain ? "SUCCESSFULLY COMPRESSED: " + target_name + " (saved " + format_bytes(before - after) + ")"
                                           : "NO GAIN: " + target_name;
                    if (gain && dry_run) msg = "SIMULATED COMPRESSION: " + target_name + " (would save " + format_bytes(before - after) + ")";
                    child->row.size = before;
                    child->row.size_after = after;
                    child->row.status = gain ? (dry_run ? Status::Simulated : Status::Completed) : Status::NoGain;
                    child->logs.push_back(msg);
                    settle_contents(*child);
                    t.parent->logs.push_back("[CHILD] " + msg);
                    global("[CHILD] " + msg);
                    ++version;
                } else {
                    return;
                }
            }
            changed();
        }

        void record_stat(const Node& node, const fs::path& path, const std::uint64_t before, const std::uint64_t after) {
            const auto now = Clock::now();
            auto since = run_start;
            if (const auto it = start_times.find(path.native()); it != start_times.end()) since = it->second;
            else if (const auto at = analyze_times.find(path.native()); at != analyze_times.end()) since = at->second;
            StatRecord r;
            r.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            r.extension = node.row.extension;
            r.original_size = before;
            r.compressed_size = after;
            r.duration_ms = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(now - since).count());
            r.run_id = run_id;
            r.file_name = node.row.name;
            run_records.push_back(std::move(r));
        }

        void on_error(const fs::path& path, const std::optional<fs::path>& parent, const std::string& message,
                      const bool is_container_self) {
            {
                std::lock_guard lock(mtx);
                auto t = resolve(path, parent);
                if (!t.found) {
                    ++unmatched;
                    return;
                }
                const auto msg = "ERROR [" + to_utf8(path.filename()) + "]: " + message;
                // a failed recompression of a container's own stream isn't final: its rebuild still runs
                if (t.root) {
                    t.node->row.status = Status::Error;
                    t.node->row.error = message;
                    t.node->logs.push_back(msg);
                    global(msg);
                    if (!is_container_self) settle_contents(*t.node);
                } else if (Node* child = child_of(t, path)) {
                    child->row.status = Status::Error;
                    child->row.error = message;
                    child->logs.push_back(msg);
                    if (!is_container_self) settle_contents(*child);
                    t.parent->logs.push_back("[CHILD] " + msg);
                    global("[CHILD] " + msg);
                }
                ++version;
            }
            changed();
        }

        void on_skipped(const fs::path& path, const std::optional<fs::path>& parent, const chisel::SkipReason reason,
                        const std::string& message, const bool is_container_self) {
            // files that are supported but were left alone stay visible; the rest is what "hide unsupported" hides
            bool hide = false;
            Status status = Status::Skipped;
            SkipNote note = SkipNote::None;
            switch (reason) {
                case chisel::SkipReason::NoImprovement:
                    status = Status::NoGain;
                    break;
                case chisel::SkipReason::Interrupted:
                    status = Status::Stopped;
                    break;
                case chisel::SkipReason::ReadOnly:
                    note = SkipNote::ReadOnly;
                    break;
                case chisel::SkipReason::ChangedDuringRun:
                    note = SkipNote::ChangedDuringRun;
                    break;
                case chisel::SkipReason::Signed:
                    break;
                case chisel::SkipReason::Junk:
                case chisel::SkipReason::Empty:
                case chisel::SkipReason::Unsupported:
                case chisel::SkipReason::NestingTooDeep:
                case chisel::SkipReason::NothingToDo:
                    hide = hide_unsupported;
                    break;
            }
            const auto kind = static_cast<SkipKind>(static_cast<int>(reason) + 1);
            {
                std::lock_guard lock(mtx);
                auto t = resolve(path, parent);
                if (!t.found) {
                    ++unmatched;
                    return;
                }
                const auto target_name = to_utf8(path.filename());
                const auto msg = reason == chisel::SkipReason::Interrupted ? "INTERRUPTED [" + target_name + "]"
                                                                            : "SKIPPED [" + target_name + "]: " + message;
                // a container's own stream being skipped isn't final: its rebuild still decides
                if (t.root) {
                    t.node->row.status = status;
                    t.node->row.note = note;
                    t.node->row.skip = kind;
                    t.node->row.skip_message = message;
                    t.node->logs.push_back(msg);
                    global(msg);
                    if (!is_container_self) settle_contents(*t.node);
                } else if (hide) {
                    if (t.node) remove_subtree(t.node->row.id);
                } else if (Node* child = child_of(t, path)) {
                    child->row.status = status;
                    child->row.note = note;
                    child->row.skip = kind;
                    child->row.skip_message = message;
                    child->logs.push_back(msg);
                    if (!is_container_self) settle_contents(*child);
                    t.parent->logs.push_back("[CHILD] " + msg);
                    global("[CHILD] " + msg);
                }
                ++version;
            }
            changed();
        }

        void on_finalize_start(const fs::path& path) {
            {
                std::lock_guard lock(mtx);
                if (phase == Phase::Analyzing) phase = Phase::Recompressing;
                const auto it = run_index.find(path.native());
                if (it == run_index.end()) return;
                Node& node = nodes.at(it->second);
                if (node.row.parent == 0) {
                    node.logs.emplace_back("RE-ASSEMBLING CONTAINER...");
                    global(node.row.name + " - RE-ASSEMBLING CONTAINER...");
                } else {
                    node.logs.emplace_back("[CHILD] RE-ASSEMBLING CONTAINER...");
                }
                ++version;
            }
            changed();
        }

        // --- placement of a queued file's final result, before its row is updated

        const RootInfo* run_root(const fs::path& path) const {
            const auto it = run_roots.find(path.native());
            return it == run_roots.end() ? nullptr : &it->second;
        }

        // a queued file's final, smaller result: put it where the output mode wants it
        void finish_root_or_child(const fs::path& path, const std::optional<fs::path>& parent, const std::uint64_t before,
                                  const std::uint64_t after, const bool is_container_self, const fs::path& destination,
                                  const Group& group) {
            Placement placement;
            const RootInfo* root = parent ? nullptr : run_root(path);
            if (root && !is_container_self && before > after && !dry_run) {
                if (!group.temp_output) {
                    placement.result = destination;
                } else {
                    // chisel doesn't guard inputs it doesn't replace: check here, as it does in place
                    const auto now = stamp_of(path);
                    if (!root->stamp || !now || *now != *root->stamp) {
                        std::error_code ec;
                        fs::remove(destination, ec);
                        chisel::Logger::log(chisel::LogLevel::Warning, "Changed during the run, left as is: " + to_utf8(path), "chisel_app");
                        on_skipped(path, parent, chisel::SkipReason::ChangedDuringRun, "Changed during the run", false);
                        return;
                    }
                    // in the output folder, the result keeps its path relative to the temp folder chisel laid it out in
                    const auto placed = group.target_folder.empty()
                        ? place_result(mode, destination, path)
                        : place_in_folder(destination, path, group.target_folder / destination.lexically_relative(group.output_dir));
                    placement.result = placed.result;
                    placement.error = placed.error;
                    if (!placed.error.empty()) {
                        chisel::Logger::log(chisel::LogLevel::Error, "Can't save the result of " + to_utf8(path) + ": " + placed.error, "chisel_app");
                    }
                }
            }
            on_finish(path, parent, before, after, is_container_self, placement);
        }

        void subscribe(chisel::EventBus& bus, const Group& group) {
            bus.subscribe<chisel::FileAnalyzeStartEvent>([this](const chisel::FileAnalyzeStartEvent& e) {
                on_analyze_start(e.path);
            });
            bus.subscribe<chisel::FileAnalyzeCompleteEvent>([this](const chisel::FileAnalyzeCompleteEvent& e) {
                on_analyze_complete(e.path, e.parent_container, e.extracted, e.num_children);
            });
            bus.subscribe<chisel::FileAnalyzeErrorEvent>([this](const chisel::FileAnalyzeErrorEvent& e) {
                on_error(e.path, e.parent_container, e.error_message, false);
            });
            bus.subscribe<chisel::FileAnalyzeSkippedEvent>([this](const chisel::FileAnalyzeSkippedEvent& e) {
                on_skipped(e.path, e.parent_container, e.reason, e.message, false);
            });
            bus.subscribe<chisel::FileProcessStartEvent>([this](const chisel::FileProcessStartEvent& e) {
                on_start(e.path, e.parent_container);
            });
            bus.subscribe<chisel::FileProcessCompleteEvent>([this, &group](const chisel::FileProcessCompleteEvent& e) {
                finish_root_or_child(e.path, e.parent_container, e.original_size, e.new_size, e.is_container, e.destination, group);
            });
            bus.subscribe<chisel::FileProcessErrorEvent>([this](const chisel::FileProcessErrorEvent& e) {
                on_error(e.path, e.parent_container, e.error_message, e.is_container);
            });
            bus.subscribe<chisel::FileProcessSkippedEvent>([this](const chisel::FileProcessSkippedEvent& e) {
                on_skipped(e.path, e.parent_container, e.reason, e.message, e.is_container);
            });
            bus.subscribe<chisel::ContainerFinalizeStartEvent>([this](const chisel::ContainerFinalizeStartEvent& e) {
                on_finalize_start(e.path);
            });
            // a container's rebuild is its final result
            bus.subscribe<chisel::ContainerFinalizeCompleteEvent>([this, &group](const chisel::ContainerFinalizeCompleteEvent& e) {
                finish_root_or_child(e.path, e.parent_container, e.original_size, e.final_size, false, e.destination, group);
            });
            bus.subscribe<chisel::ContainerFinalizeErrorEvent>([this](const chisel::ContainerFinalizeErrorEvent& e) {
                on_error(e.path, e.parent_container, e.error_message, false);
            });
            bus.subscribe<chisel::ContainerFinalizeSkippedEvent>([this](const chisel::ContainerFinalizeSkippedEvent& e) {
                on_skipped(e.path, e.parent_container, e.reason, e.message, false);
            });
        }

        // --- the run's thread

        void run(Plan plan) {
            // start() creates this thread while holding the tree lock, so the run is announced from here
            changed();
            ScopedSink sink(this);
            for (const auto& path : plan.read_only) {
                chisel::Logger::log(chisel::LogLevel::Warning, "Read-only, left as is: " + to_utf8(path), "chisel_app");
                on_skipped(path, std::nullopt, chisel::SkipReason::ReadOnly, "Read-only", false);
            }
            for (auto& group : plan.groups) {
                if (stop_requested.load()) break;
                if (group.temp_output) {
                    group.output_dir = group.target_folder.empty() ? temp_folder_for(group.inputs)
                                                                   : make_temp_folder(group.target_folder, "chisel-" + run_id);
                    if (group.output_dir.empty()) {
                        for (const auto& path : group.inputs) {
                            on_error(path, std::nullopt, group.target_folder.empty() ? "Can't create a temporary folder next to it"
                                                                                     : "Can't create a temporary folder in the output folder", false);
                        }
                        continue;
                    }
                    global("Temporary folder: " + to_utf8(group.output_dir));
                }
                {
                    chisel::EventBus bus;
                    subscribe(bus, group);
                    chisel::ProcessorExecutor current(*registry, plan.options, chisel::EncodeMode::PIPE, dry_run,
                                                      group.output_dir, bus, plan.threads);
                    {
                        std::lock_guard lock(exec_mtx);
                        executor = &current;
                        if (stop_requested.load()) current.request_stop();
                    }
                    try {
                        current.process(group.inputs, group.layout_roots);
                    } catch (const std::exception& e) {
                        push_log(static_cast<int>(chisel::LogLevel::Error), "Executor", e.what());
                    } catch (...) {
                        push_log(static_cast<int>(chisel::LogLevel::Error), "Executor", "Unknown non-standard exception");
                    }
                    std::lock_guard lock(exec_mtx);
                    executor = nullptr;
                }
                if (group.temp_output) {
                    if (!group.target_folder.empty() && plan.options.copy_unchanged) place_unchanged(group);
                    // the smaller files were moved out; what is left is chisel's own leftovers
                    std::error_code ec;
                    fs::remove_all(group.output_dir, ec);
                }
            }

            std::vector<StatRecord> records;
            {
                std::lock_guard lock(mtx);
                // files still waiting for analysis when the stop arrived get no event
                if (stop_requested.load()) {
                    for (const auto id : roots) {
                        Node& node = nodes.at(id);
                        if (node.in_run) mark_remaining_stopped(node);
                    }
                }
                for (const auto id : roots) nodes.at(id).in_run = false;
                run_index.clear();
                run_roots.clear();
                start_times.clear();
                analyze_times.clear();
                records = std::move(run_records);
                run_records.clear();
                last_records = records;
                running = false;
                stopping = false;
                phase = Phase::Idle;
                run_end = Clock::now();
                ++version;
            }
            if (stats_store && !records.empty() && !stats_store->append(records)) {
                push_log(static_cast<int>(chisel::LogLevel::Error), "chisel_app", "Can't save the statistics");
            }
            changed();
        }

        // where chisel lays out an input below its output directory (ProcessorExecutor::destination_for)
        static fs::path layout_path(const fs::path& input, const fs::path& base) {
            const auto path = normalized_path(input);
            auto relative = path.lexically_relative(base);
            if (base.empty() || relative.empty() || *relative.begin() == "..") {
                auto root = to_utf8(path.root_name());
                std::erase_if(root, [](const char c) { return c == ':' || c == '/' || c == '\\'; });
                relative = from_utf8(root) / path.relative_path();
            }
            return relative;
        }

        // OutputFolder with copy_unchanged: chisel copied the inputs it didn't improve into the temp folder
        void place_unchanged(const Group& group) {
            const auto base = common_parent(group.layout_roots);
            for (const auto& input : group.inputs) {
                const auto relative = layout_path(input, base);
                const auto copy = group.output_dir / relative;
                std::error_code ec;
                if (!fs::is_regular_file(copy, ec)) continue;
                const auto placed = place_in_folder(copy, input, group.target_folder / relative);
                const auto text = placed.error.empty() ? "COPIED UNCHANGED TO " + to_utf8(placed.result)
                                                       : "CAN'T COPY UNCHANGED: " + placed.error;
                {
                    std::lock_guard lock(mtx);
                    if (const auto root = run_roots.find(input.native()); root != run_roots.end()) {
                        if (const auto it = nodes.find(root->second.id); it != nodes.end()) {
                            it->second.logs.push_back(text);
                            ++version;
                        }
                    }
                }
                if (!placed.error.empty()) {
                    chisel::Logger::log(chisel::LogLevel::Error, "Can't copy " + to_utf8(input) + " to the output folder: " + placed.error, "chisel_app");
                }
                changed();
            }
        }

        // on the inputs' volume, so that putting results in place is a rename: their common folder, else the first one's
        fs::path temp_folder_for(const std::vector<fs::path>& inputs) const {
            const auto tag = "chisel-" + run_id;
            if (const auto common = common_parent(inputs); !common.empty()) {
                if (auto dir = make_temp_folder(common, tag); !dir.empty()) return dir;
            }
            if (!inputs.empty()) {
                if (auto dir = make_temp_folder(inputs.front().parent_path(), tag); !dir.empty()) return dir;
            }
            std::error_code ec;
            return make_temp_folder(fs::temp_directory_path(ec), tag);
        }
    };

    Session::Session(StatsStore* stats_store) : impl_(std::make_unique<Impl>(stats_store)) {}

    Session::~Session() {
        stop();
        wait();
    }

    void Session::set_on_changed(std::function<void()> callback) {
        std::lock_guard lock(impl_->callback_mtx);
        impl_->on_changed = std::move(callback);
    }

    bool Session::is_supported(const fs::path& file) const {
        try {
            if (!impl_->registry->find_by_mime(chisel::MimeDetector::detect(file)).empty()) return true;
        } catch (const std::exception&) {
            // unreadable: the extension may still tell
        }
        return !impl_->registry->find_by_extension(to_utf8(file.extension())).empty();
    }

    std::vector<std::string> Session::supported_extensions() const {
        std::set<std::string> all;
        for (const auto& processor : impl_->registry->all()) {
            for (const auto ext : processor->get_supported_extensions()) all.emplace(ext);
        }
        return {all.begin(), all.end()};
    }

    Session::AddResult Session::add_paths(const std::vector<fs::path>& paths, const bool recursive) {
        struct Found {
            fs::path path;
            fs::path import_root;
            bool supported = false;
            std::uint64_t size = 0;
        };
        std::vector<Found> found;
        for (const auto& input : paths) {
            const auto root = normalized_path(input);
            std::error_code ec;
            if (fs::is_directory(root, ec)) {
                // hidden files and folders are left out, as ChiselSwift's .skipsHiddenFiles does
                const auto opts = fs::directory_options::skip_permission_denied;
                if (recursive) {
                    fs::recursive_directory_iterator it(root, opts, ec), end;
                    for (; !ec && it != end; it.increment(ec)) {
                        if (is_hidden(*it)) {
                            if (it->is_directory(ec)) it.disable_recursion_pending();
                            continue;
                        }
                        if (it->is_regular_file(ec)) found.push_back({normalized_path(it->path()), root});
                    }
                } else {
                    fs::directory_iterator it(root, opts, ec), end;
                    for (; !ec && it != end; it.increment(ec)) {
                        if (!is_hidden(*it) && it->is_regular_file(ec)) found.push_back({normalized_path(it->path()), root});
                    }
                }
            } else if (fs::is_regular_file(root, ec)) {
                found.push_back({root, root});
            }
        }
        for (auto& f : found) {
            f.supported = is_supported(f.path);
            std::error_code ec;
            const auto size = fs::file_size(f.path, ec);
            f.size = ec ? 0 : size;
        }

        AddResult result;
        {
            std::lock_guard lock(impl_->mtx);
            for (auto& f : found) {
                const auto key = f.path.native();
                if (!f.supported) {
                    ++result.unsupported;
                    if (impl_->unsupported_set.insert(key).second) impl_->unsupported.push_back(f.path);
                    continue;
                }
                if (const auto it = impl_->root_index.find(key); it != impl_->root_index.end()) {
                    auto& node = impl_->nodes.at(it->second);
                    if (node.in_run || node.row.status == Status::Pending) {
                        ++result.duplicates;
                        continue;
                    }
                    for (const auto child : std::vector(node.children)) impl_->remove_subtree(child);
                    node.row.status = Status::Pending;
                    node.row.note = SkipNote::None;
                    node.row.skip = SkipKind::None;
                    node.row.skip_message.clear();
                    node.row.error.clear();
                    node.row.result.clear();
                    node.row.size = f.size;
                    node.row.size_after.reset();
                    node.row.container = false;
                    node.logs.clear();
                    node.import_root = f.import_root;
                    ++result.requeued;
                    continue;
                }
                const auto id = impl_->next_id++;
                auto& node = impl_->nodes[id];
                node.row.id = id;
                node.row.path = f.path;
                node.row.name = to_utf8(f.path.filename());
                node.row.extension = lower_extension(f.path);
                node.row.size = f.size;
                node.import_root = f.import_root;
                impl_->roots.push_back(id);
                impl_->root_index[key] = id;
                ++result.added;
            }
            ++impl_->version;
        }
        impl_->changed();
        return result;
    }

    bool Session::remove(const std::uint64_t id) {
        {
            std::lock_guard lock(impl_->mtx);
            const auto it = impl_->nodes.find(id);
            if (it == impl_->nodes.end() || it->second.row.parent != 0 || it->second.in_run) return false;
            impl_->root_index.erase(it->second.row.path.native());
            std::erase(impl_->roots, id);
            impl_->remove_subtree(id);
            ++impl_->version;
        }
        impl_->changed();
        return true;
    }

    void Session::clear() {
        {
            std::lock_guard lock(impl_->mtx);
            for (const auto id : std::vector(impl_->roots)) {
                const auto& node = impl_->nodes.at(id);
                if (node.in_run) continue;
                impl_->root_index.erase(node.row.path.native());
                std::erase(impl_->roots, id);
                impl_->remove_subtree(id);
            }
            impl_->unsupported.clear();
            impl_->unsupported_set.clear();
            ++impl_->version;
        }
        impl_->changed();
    }

    StartError Session::start(const Settings& settings, const RunOptions options) {
        Impl::Plan plan;
        plan.options.iterations = settings.iterations;
        plan.options.iterations_large = settings.iterations_large;
        plan.options.preserve_metadata = settings.preserve_metadata;
        plan.options.verify_checksums = settings.verify_checksums;
        plan.options.break_signatures = settings.break_signatures;
        plan.threads = std::max(1u, settings.threads);
        const auto mode = settings.output_mode;
        const bool dry = options.dry_run;
        // everything but Overwrite: chisel writes into a temp folder and only the smaller files are placed
        const bool temp_output = !dry && mode != OutputMode::Overwrite;
        plan.options.jpeg_progressive = settings.progressive_jpeg;
        plan.options.flac_exhaustive = settings.exhaustive_flac;
        // unchanged copies are only wanted in the output folder; elsewhere they would be a wasted copy into the temp folder
        plan.options.copy_unchanged = temp_output && mode == OutputMode::OutputFolder && settings.copy_unchanged;

        {
            std::thread finished;
            {
                std::lock_guard lock(impl_->mtx);
                if (impl_->running) return StartError::AlreadyRunning;
                finished = std::move(impl_->worker);
            }
            // the previous run's thread is past its last state change
            if (finished.joinable()) finished.join();
        }

        // one critical section from here on, so no row can change between choosing the files and starting
        std::lock_guard lock(impl_->mtx);
        if (impl_->running || impl_->worker.joinable()) return StartError::AlreadyRunning;
        struct Pending {
            std::uint64_t id;
            fs::path path;
            fs::path import_root;
        };
        std::vector<Pending> pending;
        for (const auto id : impl_->roots) {
            const auto& node = impl_->nodes.at(id);
            if (node.row.status == Status::Pending) pending.push_back({id, node.row.path, node.import_root});
        }
        if (pending.empty()) return StartError::NothingPending;

        std::vector<fs::path> layout_roots;
        if (mode == OutputMode::OutputFolder && !dry) {
            if (settings.output_folder.empty()) return StartError::NoOutputFolder;
            std::error_code ec;
            fs::create_directories(settings.output_folder, ec);
            if (!fs::is_directory(settings.output_folder, ec)) return StartError::OutputFolderUnavailable;
            // chisel lays results out relative to the deepest folder holding everything that was added
            std::unordered_set<Key> seen;
            for (const auto& p : pending) {
                if (seen.insert(p.import_root.native()).second) layout_roots.push_back(p.import_root);
            }
            const auto base = common_parent(layout_roots);
            if (!base.empty() && fs::equivalent(base, settings.output_folder, ec)) return StartError::OutputFolderIsInputFolder;
        }

        // what chisel checks only for files it replaces in place: a change during the run in every placing mode, and a
        // writable folder where the result goes next to the original (a read-only original is kept under its new name)
        std::unordered_map<Key, Impl::RootInfo> run_roots;
        std::vector<fs::path> runnable;
        for (const auto& p : pending) {
            Impl::RootInfo info{.id = p.id, .stamp = std::nullopt};
            if (temp_output) {
                const bool writable = mode == OutputMode::OutputFolder || folder_writable(p.path.parent_path());
                if (!writable) {
                    plan.read_only.push_back(p.path);
                } else {
                    info.stamp = stamp_of(p.path);
                    runnable.push_back(p.path);
                }
            } else {
                runnable.push_back(p.path);
            }
            run_roots[p.path.native()] = info;
        }

        if (temp_output && mode == OutputMode::OutputFolder) {
            if (!runnable.empty()) {
                Impl::Group group;
                group.inputs = runnable;
                group.layout_roots = layout_roots;
                group.target_folder = settings.output_folder;
                group.temp_output = true;
                plan.groups.push_back(std::move(group));
            }
        } else if (temp_output) {
            // one run per volume, each with its temp folder on that volume
            std::vector<std::string> order;
            std::unordered_map<std::string, Impl::Group> by_volume;
            for (const auto& path : runnable) {
                const auto key = volume_key(path);
                auto [it, inserted] = by_volume.try_emplace(key);
                if (inserted) order.push_back(key);
                it->second.inputs.push_back(path);
            }
            for (const auto& key : order) {
                auto& group = by_volume.at(key);
                group.layout_roots = group.inputs;
                group.temp_output = true;
                plan.groups.push_back(std::move(group));
            }
        } else if (!runnable.empty()) {
            Impl::Group group;
            group.inputs = runnable;
            group.layout_roots = runnable;
            plan.groups.push_back(std::move(group));
        }

        impl_->run_ids.clear();
        for (const auto& p : pending) {
            impl_->nodes.at(p.id).in_run = true;
            impl_->run_index[p.path.native()] = p.id;
            impl_->run_ids.push_back(p.id);
        }
        impl_->phase = Phase::Analyzing;
        impl_->threads = plan.threads;
        impl_->run_roots = std::move(run_roots);
        impl_->running = true;
        impl_->stopping = false;
        impl_->ran = true;
        impl_->dry_run = dry;
        impl_->mode = mode;
        impl_->hide_unsupported = settings.hide_unsupported;
        impl_->run_id = make_run_id();
        impl_->run_records.clear();
        impl_->unmatched = 0;
        impl_->run_start = Clock::now();
        impl_->stop_requested.store(false);
        ++impl_->version;
        impl_->worker = std::thread([impl = impl_.get(), plan = std::move(plan)]() mutable { impl->run(std::move(plan)); });
        return StartError::None;
    }

    void Session::stop() {
        {
            std::lock_guard lock(impl_->mtx);
            if (!impl_->running || impl_->stopping) return;
            impl_->stopping = true;
            ++impl_->version;
        }
        impl_->stop_requested.store(true);
        {
            std::lock_guard lock(impl_->exec_mtx);
            if (impl_->executor) impl_->executor->request_stop();
        }
        impl_->changed();
    }

    void Session::wait() {
        std::thread worker;
        {
            std::lock_guard lock(impl_->mtx);
            worker = std::move(impl_->worker);
        }
        if (worker.joinable()) worker.join();
    }

    bool Session::running() const {
        std::lock_guard lock(impl_->mtx);
        return impl_->running;
    }

    Snapshot Session::snapshot() const {
        // cleared first: a change made while copying notifies again rather than being missed
        impl_->notify_pending.store(false);
        Snapshot snap;
        std::lock_guard lock(impl_->mtx);
        snap.version = impl_->version;
        snap.unsupported = impl_->unsupported;
        auto& stats = snap.stats;
        const auto visit = [&](const auto& self, const std::uint64_t id, const unsigned depth) -> void {
            const auto& node = impl_->nodes.at(id);
            Row row = node.row;
            row.depth = depth;
            row.child_count = node.children.size();
            snap.rows.push_back(std::move(row));
            for (const auto child : node.children) self(self, child, depth + 1);
        };
        for (const auto id : impl_->roots) {
            const auto& row = impl_->nodes.at(id).row;
            ++stats.total;
            switch (row.status) {
                case Status::Pending: ++stats.pending; break;
                case Status::Processing: ++stats.processing; break;
                case Status::NoGain: ++stats.no_gain; break;
                case Status::Skipped: ++stats.skipped; break;
                case Status::Completed: ++stats.completed; break;
                case Status::Simulated: ++stats.simulated; break;
                case Status::Error: ++stats.errors; break;
                case Status::Stopped: ++stats.stopped; break;
            }
            if ((row.status == Status::Completed || row.status == Status::Simulated) && row.size_after && *row.size_after < row.size) {
                stats.bytes_before += row.size;
                stats.bytes_saved += row.size - *row.size_after;
            }
            visit(visit, id, 0);
        }
        for (const auto id : impl_->run_ids) {
            const auto it = impl_->nodes.find(id);
            if (it == impl_->nodes.end()) continue;
            const auto& row = it->second.row;
            ++stats.run_total;
            stats.run_bytes_total += row.size;
            switch (row.status) {
                case Status::Pending:
                case Status::Processing: break;
                case Status::NoGain: ++stats.run_no_gain; break;
                case Status::Skipped: ++stats.run_skipped; break;
                case Status::Completed: ++stats.run_completed; break;
                case Status::Simulated: ++stats.run_simulated; break;
                case Status::Error:
                    ++stats.run_errors;
                    if (stats.first_error == 0) stats.first_error = id;
                    break;
                case Status::Stopped: ++stats.run_stopped; break;
            }
            if (row.status != Status::Pending && row.status != Status::Processing) ++stats.run_done;
            if ((row.status == Status::Completed || row.status == Status::Simulated) && row.size_after && *row.size_after < row.size) {
                stats.run_bytes_saved += row.size - *row.size_after;
            }
        }
        stats.threads = impl_->threads;
        stats.dry_run = impl_->dry_run;
        stats.phase = impl_->phase;
        stats.running = impl_->running;
        stats.stopping = impl_->stopping;
        if (impl_->ran) {
            const auto end = impl_->running ? Clock::now() : impl_->run_end;
            stats.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - impl_->run_start);
        }
        return snap;
    }

    std::vector<std::string> Session::row_logs(const std::uint64_t id) const {
        std::lock_guard lock(impl_->mtx);
        const auto it = impl_->nodes.find(id);
        return it == impl_->nodes.end() ? std::vector<std::string>{} : it->second.logs;
    }

    std::vector<LogLine> Session::logs_since(const std::uint64_t seq) const {
        std::lock_guard lock(impl_->log_mtx);
        std::vector<LogLine> out;
        for (const auto& line : impl_->logs) {
            if (line.seq > seq) out.push_back(line);
        }
        return out;
    }

    std::vector<StatRecord> Session::last_run_records() const {
        std::lock_guard lock(impl_->mtx);
        return impl_->last_records;
    }

    std::string Session::last_run_id() const {
        std::lock_guard lock(impl_->mtx);
        return impl_->run_id;
    }

    std::uint64_t Session::unmatched_events() const {
        std::lock_guard lock(impl_->mtx);
        return impl_->unmatched;
    }

} // namespace chisel_app
