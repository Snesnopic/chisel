//
// Created by Giuseppe Francione on 18/09/25.
//

#include <iostream>
#include <filesystem>
#include <csignal>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <fstream>
#include "utils/color.hpp"
#include "cli/cli_parser.hpp"
#include "cli/CLI11.hpp"
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

// simple progress bar printer
inline void print_progress_bar(const size_t done, const size_t total, const double elapsed_seconds) {
    const unsigned term_width = get_terminal_width();
    const unsigned int bar_width = std::max(10u, term_width > 40u ? term_width - 40u : 20u);

    const double progress = total ? static_cast<double>(done) / static_cast<double>(total) : (done > 0 ? 1.0 : 0.0);
    const unsigned pos = static_cast<unsigned>(bar_width * progress);

    double percent = progress * 100.0;
    if (done < total && percent >= 99.95) {
        percent = 99.9;
    }
    if (done == total) {
        percent = 100.0;
    }

    std::cerr << "\r[";
    for (unsigned i = 0; i < bar_width; ++i) {
        if (i < pos) std::cerr << "=";
        else if (i == pos && done < total) std::cerr << ">";
        else if (i == pos && done == total) std::cerr << "=";
        else std::cerr << " ";
    }
    std::cerr << "] "
              << std::setw(5) << std::fixed << std::setprecision(1) << percent << "%"
              << " (" << done << "/" << total << ")"
              << " elapsed: " << std::fixed << std::setprecision(1) << elapsed_seconds << "s"
              << std::flush;
}

using namespace chisel;
namespace fs = std::filesystem;

static std::atomic<bool> interrupted{false};
static chisel::ProcessorExecutor* g_executor = nullptr;

// handle ctrl+c or termination signals
void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        std::cerr << CYAN
                  << "\n[INTERRUPT] Stop detected. Waiting for threads to finish..."
                  << RESET << std::endl;
        if (g_executor) {
            g_executor->request_stop();
        }
        interrupted.store(true);
    }
}

inline void init_utf8_locale() {
    std::setlocale(LC_ALL, "");

    const char *cur = std::setlocale(LC_CTYPE, nullptr);
    if (cur && std::string(cur).find("UTF-8") != std::string::npos) {
        Logger::log(LogLevel::Debug, std::string("Current locale: ") + cur, "LocaleInit");
        return; // ok
    }

    constexpr const char *fallbacks[] = {"C.UTF-8", "en_US.UTF-8", ".UTF-8" /* Windows */};
    for (const auto fb: fallbacks) {
        if (std::setlocale(LC_ALL, fb)) {
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

    std::signal(SIGINT, signal_handler);
    init_utf8_locale();

    try {
        if (settings.regenerate_magic) {
            MimeDetector::ensure_magic_installed();
        }
    } catch (const std::exception& e) {
        // log if magic file init fails
        Logger::log(LogLevel::Error, "Failed to initialize magic file: " + std::string(e.what()), "main");
        // this is often non-fatal, so we continue
    }

    // set console logger
    // auto sink = std::make_unique<ConsoleLogSink>();
    // sink->log_level = Logger::string_to_level(settings.log_level);
    // Logger::set_sink(std::move(sink));

    // set file logger
    Logger::clear_sinks();
    auto fileSink = std::make_unique<FileLogSink>("chisel.log",false);
    Logger::add_sink(std::move(fileSink));

    if (!settings.quiet) {
        auto consoleSink = std::make_unique<ConsoleLogSink>();
        consoleSink->log_level = Logger::string_to_level(settings.log_level);
        Logger::add_sink(std::move(consoleSink));
    }

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
        return 1;
    }

    // progress tracking
    size_t total = inputs.size();
    std::atomic<size_t> done{0};
    auto start_total = std::chrono::steady_clock::now();

    // subscribe to events: print progress and collect results
    // bus.subscribe<FileAnalyzeStartEvent>([](const FileAnalyzeStartEvent& e) {
    //     std::cerr << "[ANALYZE] " << e.path.filename().string() << std::endl;
    // });

    // update total if a container is extracted (finalization step counts as extra work)
    bus.subscribe<FileAnalyzeCompleteEvent>([&](const FileAnalyzeCompleteEvent& e) {
        if (e.extracted) {
            total += e.num_children;
        }
    });

    // generic handler for "finished" events to update progress bar
    auto on_finish = [&](auto&&) {
        const size_t current = ++done;
        const double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start_total).count();
        if (!settings.quiet) {
            print_progress_bar(current, total, elapsed);
        }
    };

    bus.subscribe<FileProcessCompleteEvent>([&](const FileProcessCompleteEvent& e) {
        if (!settings.quiet) {
            std::string status_msg;
            if (!e.replaced) {
                status_msg = settings.dry_run ? " [DRY-RUN]" : " [kept]";
            } else {
                status_msg = settings.dry_run ? " [DRY-RUN]" :
                             (settings.output_path.empty() ? " [replaced]" : " [OK]");
            }

            std::cerr
                << (e.replaced ? GREEN : YELLOW)
                << "\n[DONE] " << e.path.filename().string()
                << " (" << e.original_size << " -> " << e.new_size << " bytes)"
                << status_msg
                << RESET << std::endl;
        }
        Result r;
        r.path = e.path;
        r.mime = MimeDetector::detect(e.path);
        r.size_before = e.original_size;
        r.size_after = e.new_size;
        r.success = true;
        r.replaced = e.replaced;
        r.seconds = static_cast<double>(e.duration.count()) / 1000.0;
        results.push_back(std::move(r));

        on_finish(e);
    });

    bus.subscribe<FileProcessErrorEvent>([&](const FileProcessErrorEvent& e) {
        Logger::log(LogLevel::Error, e.path.filename().string() + " " + e.error_message, "main");

        Result r;
        r.path = e.path;
        r.mime = MimeDetector::detect(e.path);
        r.success = false;
        r.error_msg = e.error_message;
        results.push_back(std::move(r));

        on_finish(e);
    });

    bus.subscribe<FileProcessSkippedEvent>(on_finish);

    bus.subscribe<ContainerFinalizeCompleteEvent>([&](const ContainerFinalizeCompleteEvent& e) {
        auto it = std::find_if(results.begin(), results.end(),
                             [&](const Result& r){ return r.path == e.path; });
        if (it != results.end()) {
            it->size_after = e.final_size;
        }

        ContainerResult c;
        c.filename = e.path;
        c.success = true;
        c.size_after = e.final_size;
        container_results.push_back(std::move(c));

    });

    bus.subscribe<ContainerFinalizeErrorEvent>([&](const ContainerFinalizeErrorEvent& e) {
        Logger::log(LogLevel::Error, e.path.filename().string() + " " + e.error_message, "main");

        ContainerResult c;
        c.filename = e.path;
        c.success = false;
        c.error_msg = e.error_message;
        container_results.push_back(std::move(c));

        on_finish(e);
    });

    std::filesystem::path executor_output_dir;
    if (!settings.is_pipe && !settings.output_path.empty()) {
        executor_output_dir = settings.output_path;
    }

    // build executor
    ProcessorExecutor executor(registry,
                                settings.should_preserve_metadata(),
                        settings.unencodable_target_format.value_or(ContainerFormat::Unknown),
                               settings.verify_checksums,
                               settings.encode_mode,
                               settings.dry_run,
                               executor_output_dir,
                               bus,
                               interrupted,
                               settings.num_threads);
    g_executor = &executor;
    // run processing
    executor.process(inputs);
    g_executor = nullptr;

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

    if (interrupted.load()) {
        return 130; // standard exit code for SIGINT
    }
    return 0;
}