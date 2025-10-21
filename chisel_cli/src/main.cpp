//
// Created by Giuseppe Francione on 18/09/25.
//

#include <iostream>
#include <filesystem>
#include <csignal>
#include <atomic>
#include <chrono>
#include <format>
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

// simple progress bar printer
inline void print_progress_bar(const size_t done, const size_t total, double elapsed_seconds) {
    const unsigned term_width = get_terminal_width();
    const unsigned int bar_width = std::max(10u, term_width > 40u ? term_width - 40u : 20u);

    const double progress = total ? static_cast<double>(done) / static_cast<double>(total) : 0.0;
    const unsigned pos = static_cast<int>(bar_width * progress);

    std::cout << "\r[";
    for (unsigned i = 0; i < bar_width; ++i) {
        if (i < pos) std::cout << "=";
        else if (i == pos) std::cout << ">";
        else std::cout << " ";
    }
    std::cout << "] "
              << std::format("{:3.0f}%", progress * 100.0)
              << " (" << done << "/" << total << ")"
              << " elapsed: " << std::format("{:.1f}s", elapsed_seconds)
              << std::flush;
}

using namespace chisel;
namespace fs = std::filesystem;

static std::atomic<bool> interrupted{false};

// handle ctrl+c or termination signals
void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        Logger::log(LogLevel::Warning, "Stop detected. Waiting for threads to finish...", "main");
        interrupted.store(true);
    }
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signal_handler);

    Settings settings;
    try {
        // parse CLI arguments
        if (!parse_arguments(argc, argv, settings)) {
            return 1;
        }
    } catch (const std::exception& e) {
        Logger::log(LogLevel::Error, e.what(), "main");
        return 1;
    }

    // set console logger
    auto sink = std::make_unique<ConsoleLogSink>();
    sink->log_level = Logger::string_to_level(settings.log_level);
    Logger::set_sink(std::unique_ptr<ILogSink>(sink.get()));

    // registry of processors and event bus
    ProcessorRegistry registry;
    EventBus bus;

    // results collected for reporting
    std::vector<Result> results;
    std::vector<ContainerResult> container_results;

    // collect input files
    auto inputs = collect_input_files(settings.inputs, settings.recursive, settings.is_pipe);
    if (inputs.empty()) {
        Logger::log(LogLevel::Error, "No valid input files.", "main");
        return 1;
    }

    // progress tracking
    size_t total = inputs.size();
    std::atomic<size_t> done{0};
    auto start_total = std::chrono::steady_clock::now();

    // subscribe to events: print progress and collect results
    bus.subscribe<FileAnalyzeStartEvent>([](const FileAnalyzeStartEvent& e) {
        std::cout << "[ANALYZE] " << e.path << std::endl;
    });

    // update total if a container is extracted (finalization step counts as extra work)
    bus.subscribe<FileAnalyzeCompleteEvent>([&](const FileAnalyzeCompleteEvent& e) {
        if (e.extracted) {
            ++total;
        }
    });

    // generic handler for "finished" events to update progress bar
    auto on_finish = [&](auto&&) {
        const size_t current = ++done;
        const double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start_total).count();
        print_progress_bar(current, total, elapsed);
    };

    bus.subscribe<FileProcessCompleteEvent>([&](const FileProcessCompleteEvent& e) {
        std::cout << "[DONE] " << e.path
                  << " (" << e.original_size << " -> " << e.new_size << " bytes)"
                  << (e.replaced ? " [replaced]" : " [kept]") << std::endl;

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
        std::cerr << "[ERROR] " << e.path << ": " << e.error_message << std::endl;

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
        std::cout << "[FINALIZED] " << e.path
                  << " (" << e.final_size << " bytes)" << std::endl;

        ContainerResult c;
        c.filename = e.path;
        c.success = true;
        c.size_after = e.final_size;
        container_results.push_back(std::move(c));

        on_finish(e);
    });

    bus.subscribe<ContainerFinalizeErrorEvent>([&](const ContainerFinalizeErrorEvent& e) {
        std::cerr << "[ERROR FINALIZE] " << e.path << ": " << e.error_message << std::endl;

        ContainerResult c;
        c.filename = e.path;
        c.success = false;
        c.error_msg = e.error_message;
        container_results.push_back(std::move(c));

        on_finish(e);
    });

    // build executor
    ProcessorExecutor executor(registry,
                               settings.preserve_metadata,
                               settings.unencodable_target_format.value_or(ContainerFormat::Unknown),
                               settings.verify_checksums,
                               settings.encode_mode,
                               bus,
                               settings.num_threads);

    // run processing
    executor.process(inputs);

    auto end_total = std::chrono::steady_clock::now();
    double total_seconds = std::chrono::duration<double>(end_total - start_total).count();

    // export CSV if requested
    if (!settings.output_csv.empty()) {
        export_csv_report(results,
                          container_results,
                          settings.output_csv,
                          total_seconds,
                          settings.encode_mode);
    }

    if (interrupted.load()) {
        return 130; // standard exit code for SIGINT
    }
    return 0;
}