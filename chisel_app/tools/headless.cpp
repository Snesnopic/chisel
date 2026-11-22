// command-line driver of chisel_app: runs the GUI core on real files and prints the row tree and the statistics

#include "chisel_app/formats.hpp"
#include "chisel_app/output.hpp"
#include "chisel_app/paths.hpp"
#include "chisel_app/session.hpp"
#include "chisel_app/settings.hpp"
#include "chisel_app/stats.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;
using namespace chisel_app;

namespace {

    int failures = 0;

    void check(const bool ok, const std::string& what) {
        std::cout << (ok ? "  ok   " : "  FAIL ") << what << '\n';
        if (!ok) ++failures;
    }

    int selftest(const fs::path& dir) {
        std::error_code ec;
        fs::create_directories(dir, ec);

        std::cout << "settings\n";
        const Settings defaults;
        check(defaults.iterations == 15 && defaults.iterations_large == 5, "iterations 15 / 5");
        check(defaults.threads == default_threads() && defaults.threads >= 1, "threads = half the logical cores (" + std::to_string(defaults.threads) + ")");
        check(defaults.preserve_metadata && !defaults.verify_checksums && !defaults.break_signatures, "metadata on, checksums off, signatures kept");
        check(defaults.hide_unsupported && defaults.recursive_folder_import, "hide unsupported, recursive import");
        check(defaults.output_mode == OutputMode::KeepOriginal && defaults.output_folder.empty(), "keepOriginal, no output folder");
        const auto settings_file = dir / "settings.ini";
        fs::remove(settings_file, ec);
        {
            FileKeyValueStore empty(settings_file);
            check(load_settings(empty) == defaults, "an empty store loads the defaults");
        }
        Settings custom;
        custom.iterations = 3;
        custom.iterations_large = 1;
        custom.threads = 7;
        custom.preserve_metadata = false;
        custom.verify_checksums = true;
        custom.break_signatures = true;
        custom.hide_unsupported = false;
        custom.recursive_folder_import = false;
        custom.output_mode = OutputMode::OutputFolder;
        custom.output_folder = from_utf8("/tmp/out dir/caff\xc3\xa8=x\nline");
        {
            FileKeyValueStore store(settings_file);
            save_settings(custom, store);
            check(store.flush(), "flush");
        }
        {
            FileKeyValueStore again(settings_file);
            check(load_settings(again) == custom, "saved settings load back equal (utf-8, '=' and newline in the folder)");
            again.set("iterations", "zero");
            again.set("outputMode", "bogus");
            again.set("threads", "0");
            const auto loaded = load_settings(again);
            check(loaded.iterations == 15 && loaded.output_mode == OutputMode::KeepOriginal && loaded.threads == default_threads(),
                  "invalid values fall back to the defaults");
        }

        std::cout << "names\n";
        check(side_by_side_name("/a/b/photo.png") == fs::path("/a/b/photo.compressed.png"), "photo.compressed.png");
        check(backup_name("/a/b/archive.tar.gz") == fs::path("/a/b/archive.tar.original.gz"), "archive.tar.original.gz");
        check(side_by_side_name("/a/README") == fs::path("/a/README.compressed"), "README.compressed");
        const auto taken = dir / "taken.txt";
        std::ofstream(taken) << "x";
        std::ofstream(dir / "taken-1.txt") << "x";
        check(unique_path(taken) == dir / "taken-2.txt", "unique_path skips taken-1");
        check(unique_path(dir / "free.txt") == dir / "free.txt", "unique_path keeps a free name");
        std::ofstream(dir / "from.txt") << "y";
        check(rename_no_replace(dir / "from.txt", taken).value() != 0 && fs::exists(dir / "from.txt"), "rename_no_replace refuses an existing target");

        std::cout << "stats\n";
        const auto stats_file = dir / "stats.tsv";
        fs::remove(stats_file, ec);
        FileStatsStore stats(stats_file);
        const std::vector<StatRecord> records = {
            {1763700000, "png", 1000, 600, 1500, "run-a", "a.png"},
            {1763700100, "pdf", 5000, 1000, 2500, "run-a", "tab\tname.pdf"},
            {1763800000, "png", 2000, 1900, 100, "run-b", "c.png"},
            {1761000000, "zip", 900, 800, 10, "run-c", "old.zip"},
        };
        check(stats.append({records[0], records[1]}) && stats.append({records[2], records[3]}), "append twice");
        const auto loaded = stats.load();
        check(loaded == records, "records load back equal (tab in a name)");
        const auto summary = summarize(loaded);
        check(summary.files == 4 && summary.saved_bytes == 400 + 4000 + 100 + 100 && summary.runs == 3, "summary: 4 files, 4600 bytes, 3 runs");
        check(summary.biggest && summary.biggest->extension == "pdf", "biggest save is the pdf");
        check(stats.clear() && stats.load().empty(), "clear empties the store");
        check(stats.append({records[0]}) && stats.load().size() == 1, "append after clear");
        const auto months = by_period(loaded, Period::Month);
        check(months.size() == 2 && months[0].key < months[1].key, "two months, oldest first");
        for (const auto& m : months) {
            std::cout << "       " << m.key << ": " << m.summary.files << " files, " << m.summary.saved_bytes << " bytes, in "
                      << m.summary.runs << " runs\n";
        }
        for (const auto& d : by_period(loaded, Period::Day)) {
            std::cout << "       " << d.key << ": " << d.summary.files << " files, " << d.summary.saved_bytes << " bytes\n";
        }
        std::cout << (failures ? "SELFTEST FAILED: " + std::to_string(failures) + "\n" : "SELFTEST PASSED\n");
        return failures ? 1 : 0;
    }

    // every extension chisel supports, by ChiselSwift's categories
    int formats() {
        Session session;
        const auto extensions = session.supported_extensions();
        const auto groups = group_by_kind(extensions);
        std::cout << extensions.size() << " extensions\n";
        for (const auto& [kind, list] : groups) {
            std::cout << kind_name(kind) << " (" << list.size() << "):";
            for (const auto& ext : list) std::cout << ' ' << ext;
            std::cout << '\n';
        }
        const bool complete = !groups.contains(FileKind::Unknown);
        std::cout << (complete ? "every extension has a category\n" : "SOME EXTENSIONS HAVE NO CATEGORY\n");
        return complete ? 0 : 1;
    }

    struct Options {
        Settings settings;
        bool dry_run = false;
        std::vector<fs::path> inputs;
        std::string stop_on_start;
        long stop_after_ms = -1;
        std::string append_on_start;
        std::string trigger_log = "STARTED PROCESSING";
        fs::path stats_file;
        fs::path settings_file;
        bool print_logs = false;
        bool print_global_logs = false;
    };

    std::string status_label(const Row& row) {
        std::string label(to_string(row.status));
        while (label.size() < 10) label += ' ';
        return label;
    }

    void print_tree(const Snapshot& snap) {
        for (const auto& row : snap.rows) {
            std::cout << std::string(2 * row.depth, ' ') << '[' << status_label(row) << "] " << row.name;
            if (row.container) std::cout << " {" << row.child_count << "}";
            std::cout << "  " << row.size;
            if (row.size_after) {
                std::cout << " -> " << *row.size_after;
                if (row.size > 0 && *row.size_after < row.size) {
                    char pct[32];
                    std::snprintf(pct, sizeof pct, " (-%.1f%%)", 100.0 * static_cast<double>(row.size - *row.size_after) / static_cast<double>(row.size));
                    std::cout << pct;
                }
            }
            if (row.note != SkipNote::None) std::cout << "  note: " << to_string(row.note);
            if (!row.error.empty()) std::cout << "  error: " << row.error;
            if (!row.result.empty()) std::cout << "  result: " << to_utf8(row.result);
            std::cout << '\n';
        }
        for (const auto& path : snap.unsupported) std::cout << "[unsupported] " << to_utf8(path) << '\n';
    }

    // every row's container precedes it, one level up, and child counts match
    std::string consistency_error(const Snapshot& snap) {
        std::unordered_map<std::uint64_t, std::size_t> index;
        std::unordered_map<std::uint64_t, std::size_t> children;
        for (std::size_t i = 0; i < snap.rows.size(); ++i) {
            const auto& row = snap.rows[i];
            if (!index.emplace(row.id, i).second) return "duplicate id " + std::to_string(row.id);
            if (row.parent == 0) {
                if (row.depth != 0) return "root with depth";
                continue;
            }
            const auto it = index.find(row.parent);
            if (it == index.end()) return "row " + row.name + " before its container";
            if (snap.rows[it->second].depth + 1 != row.depth) return "row " + row.name + " at the wrong depth";
            ++children[row.parent];
        }
        for (const auto& row : snap.rows) {
            if (children[row.id] != row.child_count) return "child count of " + row.name;
        }
        return {};
    }

    void print_stats(const RunStats& s) {
        std::cout << "run stats: total " << s.total << ", pending " << s.pending << ", processing " << s.processing
                  << ", completed " << s.completed << ", simulated " << s.simulated << ", noGain " << s.no_gain
                  << ", skipped " << s.skipped << ", errors " << s.errors << ", stopped " << s.stopped
                  << "; saved " << s.bytes_saved << " of " << s.bytes_before << " bytes; elapsed " << s.elapsed.count()
                  << " ms; running " << s.running << '\n';
        std::cout << "this run: " << s.run_done << " of " << s.run_total << " done, completed " << s.run_completed
                  << ", simulated " << s.run_simulated << ", noGain " << s.run_no_gain << ", skipped " << s.run_skipped
                  << ", errors " << s.run_errors << ", stopped " << s.run_stopped << "; saved " << s.run_bytes_saved
                  << " of " << s.run_bytes_total << " bytes; threads " << s.threads << (s.dry_run ? ", dry run" : "")
                  << "; first error row " << s.first_error << '\n';
    }

    int run(const Options& opt) {
        FileStatsStore stats_store(opt.stats_file.empty() ? fs::path("chisel-stats.tsv") : opt.stats_file);
        Session session(opt.stats_file.empty() ? nullptr : &stats_store);

        std::mutex mtx;
        std::condition_variable cv;
        bool changed = false;
        std::size_t callbacks = 0;
        std::set<std::thread::id> callback_threads;
        const auto main_thread = std::this_thread::get_id();
        session.set_on_changed([&] {
            std::lock_guard lock(mtx);
            changed = true;
            ++callbacks;
            callback_threads.insert(std::this_thread::get_id());
            cv.notify_one();
        });

        const auto added = session.add_paths(opt.inputs, opt.settings.recursive_folder_import);
        std::cout << "added " << added.added << ", requeued " << added.requeued << ", duplicates " << added.duplicates
                  << ", unsupported " << added.unsupported << '\n';
        const auto again = session.add_paths(opt.inputs, opt.settings.recursive_folder_import);
        std::cout << "added again: " << again.added << " added, " << again.duplicates << " duplicates\n";

        const auto t0 = std::chrono::steady_clock::now();
        const auto error = session.start(opt.settings, {.dry_run = opt.dry_run});
        if (error != StartError::None) {
            std::cout << "start refused: " << to_string(error) << '\n';
            print_tree(session.snapshot());
            return 2;
        }
        std::cout << "mode " << to_string(opt.settings.output_mode) << (opt.dry_run ? " (dry run)" : "") << ", threads "
                  << opt.settings.threads << ", run " << session.last_run_id() << '\n';

        std::size_t snapshots = 0;
        std::uint64_t last_version = 0;
        std::string inconsistency;
        bool stopped = false;
        bool appended = false;
        std::size_t added_during_run = 0;
        while (true) {
            {
                std::unique_lock lock(mtx);
                cv.wait_for(lock, std::chrono::milliseconds(200), [&] { return changed; });
                changed = false;
            }
            const auto snap = session.snapshot();
            ++snapshots;
            if (snap.version < last_version) inconsistency = "version went back";
            last_version = snap.version;
            if (const auto e = consistency_error(snap); !e.empty() && inconsistency.empty()) inconsistency = e;

            const auto started = [&](const std::string& name) -> const Row* {
                for (const auto& row : snap.rows) {
                    if (row.parent != 0 || row.name != name) continue;
                    for (const auto& line : session.row_logs(row.id)) {
                        if (line.find(opt.trigger_log) != std::string::npos) return &row;
                    }
                }
                return nullptr;
            };
            if (!opt.append_on_start.empty() && !appended) {
                if (const Row* row = started(opt.append_on_start)) {
                    std::ofstream(row->path, std::ios::binary | std::ios::app).put('\0');
                    appended = true;
                    std::cout << "appended a byte to " << row->name << " while it was processed\n";
                    // adding the same files during the run must find them all duplicates
                    added_during_run = session.add_paths(opt.inputs, opt.settings.recursive_folder_import).duplicates;
                }
            }
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            if (!stopped && ((!opt.stop_on_start.empty() && started(opt.stop_on_start)) ||
                             (opt.stop_after_ms >= 0 && elapsed >= opt.stop_after_ms))) {
                session.stop();
                stopped = true;
                std::cout << "stop requested after " << elapsed << " ms\n";
            }
            if (!snap.stats.running) break;
        }
        session.wait();
        const auto snap = session.snapshot();

        std::cout << "\n--- tree\n";
        print_tree(snap);
        std::cout << "\n--- status bar\n";
        print_stats(snap.stats);
        std::cout << "\n--- records of this run\n";
        for (const auto& r : session.last_run_records()) {
            std::cout << r.file_name << " ." << r.extension << " " << r.original_size << " -> " << r.compressed_size
                      << " in " << r.duration_ms << " ms, run " << r.run_id << '\n';
        }
        if (!opt.stats_file.empty()) {
            const auto all = stats_store.load();
            const auto summary = summarize(all);
            std::cout << "persisted: " << summary.files << " files, saved " << summary.saved_bytes << " bytes, in "
                      << summary.runs << " runs";
            if (summary.biggest) std::cout << ", biggest save ." << summary.biggest->extension << " (" << summary.biggest->saved() << " bytes)";
            std::cout << '\n';
            for (const auto& d : by_period(all, Period::Day)) std::cout << "  day " << d.key << ": " << d.summary.files << " files, " << d.summary.saved_bytes << " bytes\n";
        }
        if (opt.print_logs) {
            std::cout << "\n--- row logs\n";
            for (const auto& row : snap.rows) {
                std::cout << std::string(2 * row.depth, ' ') << row.name << ":\n";
                for (const auto& line : session.row_logs(row.id)) std::cout << std::string(2 * row.depth, ' ') << "    " << line << '\n';
            }
        }
        if (opt.print_global_logs) {
            std::cout << "\n--- log (info and above)\n";
            for (const auto& line : session.logs_since(0)) std::cout << '[' << line.level << "][" << line.tag << "] " << line.text << '\n';
        }
        std::size_t warnings = 0, errors = 0;
        for (const auto& line : session.logs_since(0)) {
            warnings += line.level == 3;
            errors += line.level == 4;
        }

        std::cout << "\n--- checks\n";
        std::cout << "unmatched events: " << session.unmatched_events() << '\n';
        std::cout << "chisel log: " << warnings << " warnings, " << errors << " errors\n";
        std::cout << "notifications: " << callbacks << " callbacks on " << callback_threads.size() << " threads (main thread among them: "
                  << (callback_threads.contains(main_thread) ? "yes" : "no") << "), " << snapshots << " snapshots, final version " << snap.version << '\n';
        if (appended) std::cout << "re-adding during the run: " << added_during_run << " duplicates\n";
        std::cout << "snapshot consistency: " << (inconsistency.empty() ? "ok" : inconsistency) << '\n';
        std::size_t left = 0;
        for (const auto& row : snap.rows) left += row.status == Status::Pending || row.status == Status::Processing;
        std::cout << "rows left pending/processing: " << left << '\n';
        return inconsistency.empty() ? 0 : 1;
    }

    void usage() {
        std::cerr << "usage: chisel_app_headless selftest DIR\n"
                 "       chisel_app_headless formats\n"
                     "       chisel_app_headless [--mode overwrite|sideBySide|keepOriginal|outputFolder] [--out DIR]\n"
                     "         [--threads N] [--iterations N] [--iterations-large N] [--dry-run] [--no-recursive]\n"
                     "         [--show-unsupported] [--verify] [--no-metadata] [--break-signatures] [--settings FILE]\n"
                     "         [--stats FILE] [--stop-on-start NAME] [--stop-after-ms N] [--append-on-start NAME]\n"
                     "         [--logs] [--global-logs] PATH...\n";
    }

} // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::string(argv[1]) == "formats") return formats();
    if (argc >= 2 && std::string(argv[1]) == "selftest") {
        if (argc < 3) {
            usage();
            return 2;
        }
        return selftest(argv[2]);
    }
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                usage();
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--settings") {
            opt.settings_file = next();
            FileKeyValueStore store(opt.settings_file);
            opt.settings = load_settings(store);
        } else if (a == "--mode") {
            const auto mode = output_mode_from_string(next());
            if (!mode) {
                usage();
                return 2;
            }
            opt.settings.output_mode = *mode;
        } else if (a == "--out") opt.settings.output_folder = next();
        else if (a == "--threads") opt.settings.threads = static_cast<unsigned>(std::stoul(next()));
        else if (a == "--iterations") opt.settings.iterations = static_cast<unsigned>(std::stoul(next()));
        else if (a == "--iterations-large") opt.settings.iterations_large = static_cast<unsigned>(std::stoul(next()));
        else if (a == "--dry-run") opt.dry_run = true;
        else if (a == "--no-recursive") opt.settings.recursive_folder_import = false;
        else if (a == "--show-unsupported") opt.settings.hide_unsupported = false;
        else if (a == "--verify") opt.settings.verify_checksums = true;
        else if (a == "--no-metadata") opt.settings.preserve_metadata = false;
        else if (a == "--break-signatures") opt.settings.break_signatures = true;
        else if (a == "--stats") opt.stats_file = next();
        else if (a == "--stop-on-start") opt.stop_on_start = next();
        else if (a == "--stop-after-ms") opt.stop_after_ms = std::stol(next());
        else if (a == "--append-on-start") opt.append_on_start = next();
        else if (a == "--trigger-log") opt.trigger_log = next();
        else if (a == "--logs") opt.print_logs = true;
        else if (a == "--global-logs") opt.print_global_logs = true;
        else if (a.starts_with("--")) {
            usage();
            return 2;
        } else opt.inputs.emplace_back(a);
    }
    if (opt.inputs.empty()) {
        usage();
        return 2;
    }
    if (!opt.settings_file.empty()) {
        FileKeyValueStore store(opt.settings_file);
        save_settings(opt.settings, store);
        store.flush();
    }
    return run(opt);
}
