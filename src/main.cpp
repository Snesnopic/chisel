//
// Created by Giuseppe Francione on 18/09/25.
//

#include <iostream>
#include <filesystem>
#include <csignal>
#include <atomic>
#include <chrono>
#include "cli/cli_parser.hpp"
#include "report/report_generator.hpp"
#include "utils/processor_registry.hpp"
#include "utils/processor_executor.hpp"
#include "utils/event_bus.hpp"
#include "utils/events.hpp"
#include "utils/console_log_sink.hpp"
#include "utils/file_scanner.hpp"
#include "utils/logger.hpp"
#include "utils/file_type.hpp"
#include "utils/mime_detector.hpp"

using namespace chisel;
namespace fs = std::filesystem;

static std::atomic<bool> interrupted{false};

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
        if (!parse_arguments(argc, argv, settings)) {
            return 1;
        }
    } catch (const std::exception& e) {
        Logger::log(LogLevel::Error, e.what(), "main");
        return 1;
    }

    // Configura logger su console
    Logger::set_sink(std::make_unique<ConsoleLogSink>());

    // Crea registry ed event bus
    ProcessorRegistry registry;
    EventBus bus;

    // Collector per report
    std::vector<Result> results;
    std::vector<ContainerResult> container_results;
    auto start_total = std::chrono::steady_clock::now();

    // Listener CLI: stampa eventi principali e accumula risultati
    bus.subscribe<FileAnalyzeStartEvent>([](const FileAnalyzeStartEvent& e) {
        std::cout << "[ANALYZE] " << e.path << std::endl;
    });
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
        r.seconds = e.duration.count() / 1000.0;
        results.push_back(std::move(r));
    });
    bus.subscribe<FileProcessErrorEvent>([&](const FileProcessErrorEvent& e) {
        std::cerr << "[ERROR] " << e.path << ": " << e.error_message << std::endl;

        Result r;
        r.path = e.path;
        r.mime = MimeDetector::detect(e.path);
        r.success = false;
        r.error_msg = e.error_message;
        results.push_back(std::move(r));
    });
    bus.subscribe<ContainerFinalizeCompleteEvent>([&](const ContainerFinalizeCompleteEvent& e) {
        std::cout << "[FINALIZED] " << e.path
                  << " (" << e.final_size << " bytes)" << std::endl;

        ContainerResult c;
        c.filename = e.path;
        c.success = true;
        c.size_after = e.final_size;
        container_results.push_back(std::move(c));
    });
    bus.subscribe<ContainerFinalizeErrorEvent>([&](const ContainerFinalizeErrorEvent& e) {
        std::cerr << "[ERROR FINALIZE] " << e.path << ": " << e.error_message << std::endl;

        ContainerResult c;
        c.filename = e.path;
        c.success = false;
        c.error_msg = e.error_message;
        container_results.push_back(std::move(c));
    });

    // Costruisci executor
    ProcessorExecutor executor(registry,
                               settings.preserve_metadata,
                               settings.unencodable_target_format.value_or(ContainerFormat::Unknown),
                               settings.verify_checksums,
                               settings.encode_mode,
                               bus,
                               settings.num_threads);

    auto inputs = collect_input_files(settings.inputs, settings.recursive, settings.is_pipe);
    if (inputs.empty()) {
        Logger::log(LogLevel::Error, "No valid input files.", "main");
        return 1;
    }

    executor.process(inputs);

    auto end_total = std::chrono::steady_clock::now();
    double total_seconds = std::chrono::duration<double>(end_total - start_total).count();

    // Se richiesto, esporta CSV
    if (!settings.output_csv.empty()) {
        export_csv_report(results,
                          container_results,
                          settings.output_csv,
                          total_seconds,
                          settings.encode_mode);
    }

    if (interrupted.load()) {
        return 130; // codice standard per SIGINT
    }
    return 0;
}