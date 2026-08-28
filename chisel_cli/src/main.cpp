//
// Created by Giuseppe Francione on 18/09/25.
//

#include <iostream>
#include <filesystem>
#include <csignal>
#include <atomic>
#include <chrono>
#include <thread>
#include <iomanip>
#include <fstream>
#include "utils/color.hpp"
#include "cli/CLI11.hpp"
#include "cli/cli_parser.hpp"
#include "report/report_generator.hpp"
#include "../../libchisel/include/processor_registry.hpp"
#include "../../libchisel/include/processor_executor.hpp"
#include "../../libchisel/include/event_bus.hpp"
#include "../../libchisel/include/events.hpp"
#include "utils/console_log_sink.hpp"
#include "utils/file_scanner.hpp"
#include "../../libchisel/include/logger.hpp"
#include "../../libchisel/include/file_type.hpp"
#include "../../libchisel/include/mime_detector.hpp"
#include "utils/file_log_sink.hpp"

static std::vector<std::string> g_active_files;

// Global mutex to synchronize console output from multiple threads
std::mutex g_console_mtx;

// Guards results/container_results, mutated concurrently from ThreadPool worker threads via EventBus
std::mutex g_results_mtx;

// Helper to clear the current line
inline void clear_line_internal() {
    const unsigned term_width = get_terminal_width();
    std::cerr << "\r" << std::string(term_width - 1, ' ') << "\r";
}

// Updated Progress bar printer that accepts status text
inline void print_progress_bar_internal(const std::size_t done, const std::size_t total, const std::string& status_text) {
    const unsigned term_width = get_terminal_width();

    // Base info length estimation (~40 chars for stats)
    const unsigned int available_width = term_width > 50 ? term_width - 1 : 40;
    unsigned int bar_width = 20;

    // Dynamic adjustment
    if (available_width > 80) bar_width = 30;
    else if (available_width < 60) bar_width = 10;

    const double progress = (total != 0U) ? static_cast<double>(done) / static_cast<double>(total) : 0.0;
    const auto pos = static_cast<unsigned>(bar_width * progress);

    double percent = progress * 100.0;
    if (done < total && percent >= 99.95) percent = 99.9;
    if (done == total && total > 0) percent = 100.0;

    std::cerr << "\r[";
    for (unsigned i = 0; i < bar_width; ++i) {
        if (i < pos) std::cerr << "=";
        else if (i == pos && done < total) std::cerr << ">";
        else if (i == pos && done == total) std::cerr << "=";
        else std::cerr << " ";
    }
    std::cerr << "] "
              << std::setw(5) << std::fixed << std::setprecision(1) << percent << "% "
              << "(" << done << "/" << total << ")";

    if (!status_text.empty()) {
        std::cerr << " " << status_text;
    }

    std::cerr << "\033[K" << std::flush; // ANSI Clear Line to right
}

using namespace chisel;
namespace fs = std::filesystem;

static std::atomic<chisel::ProcessorExecutor*> g_executor{nullptr};
static std::atomic<bool> g_stop_requested{false};

// async-signal-safe: only sets a flag, a watcher thread does the real work
void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        g_stop_requested.store(true, std::memory_order_relaxed);
    }
}

// polls the flag set by signal_handler and runs the actual stop logic on a
// normal thread, since mutexes/iostreams aren't safe to touch from a handler
void stop_watcher_loop() {
    while (!g_stop_requested.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::scoped_lock lock(g_console_mtx);
    std::cerr << CYAN
              << "\n[INTERRUPT] Stop detected. Waiting for threads to finish..."
              << RESET << std::endl;
    auto* executor_ptr = g_executor.load(std::memory_order_relaxed);
    if (executor_ptr != nullptr) {
        executor_ptr->request_stop();
    }
}

inline void init_utf8_locale() {
    std::setlocale(LC_ALL, "");
    try {
        std::locale::global(std::locale(""));
    } catch (...) {}

    const char *cur = std::setlocale(LC_CTYPE, nullptr);
    if (cur && std::string(cur).find("UTF-8") != std::string::npos) {
        Logger::log(LogLevel::Debug, std::string("Current locale: ") + cur, "LocaleInit");
        return; // ok
    }

    constexpr const char *fallbacks[] = {"C.UTF-8", "en_US.UTF-8", ".UTF-8" /* Windows */};
    for (const auto fb: fallbacks) {
        if (std::setlocale(LC_ALL, fb)) {
            try {
                std::locale::global(std::locale(fb));
            } catch(...) {}

            Logger::log(LogLevel::Info, std::string("Locale set to ") + fb, "LocaleInit");
            return;
        }
    }

    // no UTF-8 available
    Logger::log(LogLevel::Warning, "UTF-8 locale not available; non-ASCII file names may be problematic.",
                "LocaleInit");
}

int main(int argc, char* argv[]) {

    CLI::App app{"chisel: Cross-platform tool for lossless recompression."};
    Settings settings;
    setup_cli_parser(app, settings);

    try {
        app.parse(argc, argv);
    }
    catch (const CLI::CallForHelp &e) {
        return app.exit(e);
    }
    catch (const CLI::CallForVersion &e) {
        return app.exit(e);
    }
    catch (const CLI::ParseError &e) {
        std::cerr << RED << "Parse error: " << e.what() << RESET << std::endl;
        return app.exit(e);
    }

    // set console logger
    // auto sink = std::make_unique<ConsoleLogSink>();
    // sink->log_level = Logger::string_to_level(settings.log_level);
    // Logger::set_sink(std::move(sink));

    // set file logger
    Logger::clear_sinks();
    if (!settings.log_file.empty()) {
        auto fileSink = std::make_unique<FileLogSink>(settings.log_file, false);
        fileSink->log_level = Logger::string_to_level(settings.log_level);
        Logger::add_sink(std::move(fileSink));
    }

    if (!settings.quiet) {
        auto consoleSink = std::make_unique<ConsoleLogSink>();
        consoleSink->log_level = Logger::string_to_level(settings.log_level);
        Logger::add_sink(std::move(consoleSink));
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    std::thread(stop_watcher_loop).detach();
    init_utf8_locale();

    // registry of processors and event bus
    ProcessorRegistry registry;
    EventBus bus;

    // results collected for reporting
    std::vector<Result> results;
    std::vector<ContainerResult> container_results;

    // collect input files
    auto inputs = collect_input_files(settings.inputs, settings, settings.is_pipe);
    if (inputs.empty()) {
        Logger::log(LogLevel::Error, "No valid input files.", "main");
        return EXIT_FAILURE;
    }

    // progress tracking
    // overall discovered-file count (scheduled + extracted), used only for the
    // phase-1 "N files found" summary
    std::size_t total = 0;
    // phase 2 (recompression) and phase 3 (finalization) each get their own
    // done/total pair, so each phase's bar reaches 100% on its own work only
    std::size_t phase2_total = 0;
    std::atomic<std::size_t> phase2_done{0};
    std::size_t phase3_total = 0;
    std::atomic<std::size_t> phase3_done{0};
    auto start_total = std::chrono::steady_clock::now();

    // true once finalization (phase 3) has started; switches the status label
    // from "Processing: " to "Finalizing: " for the shared progress bar
    bool phase3_started = false;
    // guards the one-time phase-1 summary line ("N files found"), printed
    // right before whichever of phase 2/3 actually starts first
    bool phase1_summary_printed = false;

    // builds the "Processing: x.jpg" / "Finalizing: x.pdf" / "...: N files" status
    // text from the currently active file/container list; caller holds g_console_mtx
    auto make_status_text = [&]() -> std::string {
        if (g_active_files.empty()) return "";
        const std::string label = phase3_started ? "Finalizing: " : "Processing: ";
        if (g_active_files.size() > 1) {
            return label + std::to_string(g_active_files.size()) + " files";
        }
        return label + g_active_files.front();
    };

    // prints the "N files found" phase-1 summary exactly once; caller holds g_console_mtx
    auto ensure_phase1_summary_printed = [&]() {
        if (phase1_summary_printed) return;
        phase1_summary_printed = true;
        std::cerr << "\n" << total << " files found\n\n";
    };

    // guards the one-time "Processing files..." phase header
    bool phase2_header_printed = false;

    // subscribe to events: print progress and collect results

    // phase 1 (analysis): a permanent line for every container found (regular
    // files print nothing here), indented by nesting depth so containers found
    // inside another container are visually distinguishable from direct input
    bus.subscribe<FileAnalyzeCompleteEvent>([&](const FileAnalyzeCompleteEvent& e) {
        if (e.scheduled) {
            total++;
            phase2_total++;
        }
        if (e.extracted) {
            total++;
            phase3_total++;
        }

        if (settings.quiet || !(e.extracted && e.num_children > 0)) return;

        std::scoped_lock lock(g_console_mtx);
        std::cerr << std::string(2 + 2 * e.depth, ' ') << e.path.filename().string()
                  << " -> found " << e.num_children << " files inside" << std::endl;
    });

    // Process Start: Update the "Processing: ..." text dynamically
    bus.subscribe<FileProcessStartEvent>([&](const FileProcessStartEvent& e) {
        if (settings.quiet || e.is_container) return;

        std::scoped_lock lock(g_console_mtx);
        if (!phase2_header_printed) {
            phase2_header_printed = true;
            ensure_phase1_summary_printed();
            std::cerr << "Processing files...\n\n";
        }
        g_active_files.push_back(e.path.filename().string());

        // Force an immediate redraw of the bar with the new status
        print_progress_bar_internal(phase2_done.load(), phase2_total, make_status_text());
    });

    // generic handler for "finished" events to update progress bar; phase 2 and
    // phase 3 completions share this handler but advance their own counter, so
    // each phase's bar reaches N/N on its own work rather than the combined total
    auto on_finish = [&](const std::string& finished_filename) {
        const std::size_t current = phase3_started ? ++phase3_done : ++phase2_done;
        const std::size_t phase_total = phase3_started ? phase3_total : phase2_total;
        if (settings.quiet) return;

        std::scoped_lock lock(g_console_mtx);

        const auto it = std::find(g_active_files.begin(), g_active_files.end(), finished_filename);
        if (it != g_active_files.end()) {
            g_active_files.erase(it);
        }

        print_progress_bar_internal(current, phase_total, make_status_text());
    };

    // phase 3 (finalization): same status-bar mechanism as phase 2, with the
    // "Finalizing: " label; containers finalize one at a time, so at most one
    // entry is ever active
    bus.subscribe<ContainerFinalizeStartEvent>([&](const ContainerFinalizeStartEvent& e) {
        if (settings.quiet) return;

        std::scoped_lock lock(g_console_mtx);
        if (!phase3_started) {
            phase3_started = true;
            ensure_phase1_summary_printed();
            std::cerr << "\n\nFinalizing opened containers...\n\n";
        }
        g_active_files.push_back(e.path.filename().string());
        print_progress_bar_internal(phase3_done.load(), phase3_total, make_status_text());
    });

    bus.subscribe<FileProcessCompleteEvent>([&](const FileProcessCompleteEvent& e) {
        if (!settings.quiet && !e.is_container) {
            std::string status_msg;
            if (!e.replaced) {
                status_msg = settings.dry_run ? " [DRY-RUN]" : " [kept]";
            } else {
                status_msg = settings.dry_run ? " [DRY-RUN]" :
                             (settings.output_path.empty() ? " [replaced]" : " [OK]");
            }

            {
                std::scoped_lock lock(g_console_mtx);
                clear_line_internal();

                std::cerr
                    << (e.replaced ? GREEN : YELLOW)
                    << "[DONE] " << e.path.filename().string()
                    << " (" << e.original_size << " -> " << e.new_size << " bytes)"
                    << status_msg
                    << RESET << std::endl;
            }
        }
        Result r;
        r.path = e.path;
        r.mime = MimeDetector::detect(e.path);
        r.size_before = e.original_size;
        r.size_after = e.new_size;
        r.success = true;
        r.replaced = e.replaced;
        r.seconds = static_cast<double>(e.duration.count()) / 1000.0;
        {
            std::scoped_lock lock(g_results_mtx);
            results.push_back(std::move(r));
        }

        on_finish(e.path.filename().string());
    });

    bus.subscribe<FileProcessErrorEvent>([&](const FileProcessErrorEvent& e) {
        {
            std::scoped_lock lock(g_console_mtx);
            clear_line_internal();
            Logger::log(LogLevel::Error, e.path.filename().string() + " " + e.error_message, "main");
        }

        Result r;
        r.path = e.path;
        r.mime = MimeDetector::detect(e.path);
        r.success = false;
        r.error_msg = e.error_message;
        {
            std::scoped_lock lock(g_results_mtx);
            results.push_back(std::move(r));
        }

        on_finish(e.path.filename().string());
    });

    bus.subscribe<FileProcessSkippedEvent>([&](const FileProcessSkippedEvent& e) {
        on_finish(e.path.filename().string());
    });

    bus.subscribe<ContainerFinalizeCompleteEvent>([&](const ContainerFinalizeCompleteEvent& e) {
        if (!settings.quiet) {
            std::string status_msg;
            if (!e.replaced) {
                status_msg = settings.dry_run ? " [DRY-RUN]" : " [kept]";
            } else {
                status_msg = settings.dry_run ? " [DRY-RUN]" :
                             (settings.output_path.empty() ? " [replaced]" : " [OK]");
            }

            {
                std::scoped_lock lock(g_console_mtx);
                clear_line_internal();

                std::cerr
                    << (e.replaced ? GREEN : YELLOW)
                    << "[DONE] " << e.path.filename().string()
                    << " (" << e.original_size << " -> " << e.final_size << " bytes)"
                    << status_msg
                    << RESET << std::endl;
            }
        }

        {
            std::scoped_lock lock(g_results_mtx);
            const auto it = std::find_if(results.begin(), results.end(), [&](const Result& r){ return r.path == e.path; });
            if (it != results.end()) {
                it->size_after = e.final_size;
            }

            ContainerResult c;
            c.filename = e.path;
            c.success = true;
            c.size_before = e.original_size;
            c.size_after = e.final_size;
            container_results.push_back(std::move(c));
        }
        on_finish(e.path.filename().string());
    });

    bus.subscribe<ContainerFinalizeErrorEvent>([&](const ContainerFinalizeErrorEvent& e) {
        {
            std::lock_guard<std::mutex> lock(g_console_mtx);
            clear_line_internal();
            Logger::log(LogLevel::Error, e.path.filename().string() + " " + e.error_message, "main");
        }

        ContainerResult c;
        c.filename = e.path;
        c.success = false;
        c.error_msg = e.error_message;
        {
            std::scoped_lock lock(g_results_mtx);
            container_results.push_back(std::move(c));
        }

        on_finish(e.path.filename().string());
    });

    std::filesystem::path executor_output_dir;
    if (!settings.is_pipe && !settings.output_path.empty()) {
        executor_output_dir = settings.output_path;
    }

    // build executor
    ProcessorExecutor executor(registry,
                               settings.options,
                               settings.encode_mode,
                               settings.dry_run,
                               executor_output_dir,
                               bus,
                               settings.num_threads);
    g_executor.store(&executor);

    if (!settings.quiet) {
        std::cerr << "\nCollecting files...\n\n" << std::flush;
    }

    // run processing
    executor.process(inputs);
    g_executor.store(nullptr);

    // Final cleanup of the progress bar line: reflect whichever phase actually
    // ran last (phase 3 if any containers were finalized, else phase 2)
    if (!settings.quiet) {
        std::lock_guard<std::mutex> lock(g_console_mtx);
        const std::size_t final_total = phase3_total > 0 ? phase3_total : phase2_total;
        print_progress_bar_internal(final_total, final_total, "Completed.");
        std::cerr << std::endl;
    }

    auto end_total = std::chrono::steady_clock::now();
    double total_seconds = std::chrono::duration<double>(end_total - start_total).count();

    if (settings.is_pipe && !inputs.empty() && !settings.dry_run) {
        const fs::path& temp_file = inputs.front();

        // in pipe mode, executor_output_dir is empty, so the
        // optimized file is the temp_file itself (replaced in-place)
        std::ifstream infile(temp_file, std::ios::binary);
        if (infile) {
            std::cout << infile.rdbuf();
        }

        std::error_code ec_rename;
        fs::rename(temp_file, settings.output_path, ec_rename);
        if (ec_rename) {
            // fallback to copy if rename fails (e.g., different devices)
            std::error_code ec_copy;
            fs::copy_file(temp_file, settings.output_path, fs::copy_options::overwrite_existing, ec_copy);
            if (ec_copy) {
                std::cerr << RED << "Error: Failed to write final output to: "
                          << settings.output_path.string() << RESET << std::endl;
            } else {
                fs::remove(temp_file, ec_rename); // clean up temp
            }
        }

    } else if (settings.is_pipe && !inputs.empty()) {
        // pipe mode but with --dry-run, just clean up the temp file
        std::error_code ec;
        fs::remove(inputs.front(), ec);
    }

    // export CSV if requested
    if (!settings.report_path.empty()) {
        export_csv_report(results,
                          container_results,
                          settings.report_path,
                          total_seconds,
                          settings.encode_mode);
    }

    if (executor.is_stopped()) {
        return 130; // standard exit code for SIGINT
    }
    return EXIT_SUCCESS;
}