#ifndef CHISEL_APP_SESSION_HPP
#define CHISEL_APP_SESSION_HPP

#include "settings.hpp"
#include "stats.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace chisel_app {

    // ChiselSwift's ProcessingStatus
    enum class Status { Pending, Processing, NoGain, Skipped, Completed, Simulated, Error, Stopped };
    std::string_view to_string(Status status);

    // why a file was left as it was, when the user can do something about it (ChiselSwift's skipNote)
    enum class SkipNote { None, ReadOnly, ChangedDuringRun };
    std::string_view to_string(SkipNote note);

    // chisel's SkipReason, for rows left as they were
    enum class SkipKind { None, Junk, Empty, Unsupported, Signed, NestingTooDeep, NothingToDo, NoImprovement, Interrupted, ReadOnly, ChangedDuringRun };

    struct Row {
        std::uint64_t id = 0;          // stable for the row's life
        std::uint64_t parent = 0;      // 0 for queued files
        unsigned depth = 0;            // 0 for queued files
        std::size_t child_count = 0;
        std::filesystem::path path;    // the queued file, or where chisel extracted it
        std::filesystem::path result;  // completed: where the smaller file is
        std::string name;              // utf-8 file name
        std::string extension;         // lowercase, no dot
        Status status = Status::Pending;
        SkipNote note = SkipNote::None;
        SkipKind skip = SkipKind::None;
        std::string skip_message;      // chisel's text for the skip
        std::string error;
        std::uint64_t size = 0;
        std::optional<std::uint64_t> size_after;
        bool container = false;        // chisel extracted something from it
    };

    struct LogLine {
        std::uint64_t seq = 0;
        int level = 0;                 // chisel::LogLevel
        std::string tag;
        std::string text;
    };

    struct Snapshot {
        std::uint64_t version = 0;     // grows with every change
        std::vector<Row> rows;         // pre-order: every row is followed by what was found inside it
        std::vector<std::filesystem::path> unsupported;
        RunStats stats;
    };

    enum class StartError {
        None,
        AlreadyRunning,
        NothingPending,
        NoOutputFolder,
        OutputFolderUnavailable,       // it can't be created
        OutputFolderIsInputFolder      // chisel would write over the inputs without its in-place protections
    };
    std::string_view to_string(StartError error);

    struct RunOptions {
        bool dry_run = false;
    };

    // queue, run and row tree: chisel's threads only change state and signal "changed", the ui reads snapshot()
    class Session {
    public:
        explicit Session(StatsStore* stats_store = nullptr);
        ~Session();
        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;

        // any thread, once per snapshot(), maybe under chisel's logger lock: only wake the ui (PostMessage)
        void set_on_changed(std::function<void()> callback);

        struct AddResult {
            std::size_t added = 0;
            std::size_t requeued = 0;    // finished rows made pending again
            std::size_t duplicates = 0;  // already pending or in the run
            std::size_t unsupported = 0;
        };
        // blocking (reads file headers to detect formats) and thread-safe: a UI calls it off its own thread
        AddResult add_paths(const std::vector<std::filesystem::path>& paths, bool recursive);
        [[nodiscard]] bool is_supported(const std::filesystem::path& file) const;
        [[nodiscard]] std::vector<std::string> supported_extensions() const; // sorted, with the dot
        bool remove(std::uint64_t id);   // a queued file that isn't in the running run
        void clear();                    // everything not in the running run, and the unsupported list

        StartError start(const Settings& settings, RunOptions options = {});
        void stop();
        void wait();                     // joins the run's thread; never from the changed callback
        [[nodiscard]] bool running() const;

        [[nodiscard]] Snapshot snapshot() const;
        [[nodiscard]] std::vector<std::string> row_logs(std::uint64_t id) const;
        [[nodiscard]] std::vector<LogLine> logs_since(std::uint64_t seq) const;
        [[nodiscard]] std::vector<StatRecord> last_run_records() const;
        [[nodiscard]] std::string last_run_id() const;
        [[nodiscard]] std::uint64_t unmatched_events() const; // events naming no row nor a known container

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace chisel_app

#endif // CHISEL_APP_SESSION_HPP
