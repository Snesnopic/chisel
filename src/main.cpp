//
// Created by Giuseppe Francione on 18/09/25.
//

#include <iostream>
#include <filesystem>
#include <csignal>
#include <atomic>
#include "cli/cli_parser.hpp"
#include "utils/processor_registry.hpp"
#include "utils/processor_executor.hpp"
#include "utils/event_bus.hpp"
#include "utils/events.hpp"
#include "utils/console_log_sink.hpp"
#include "utils/file_scanner.hpp"
#include "utils/logger.hpp"
#include "utils/file_type.hpp"

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

    // Listener CLI: stampa eventi principali
    bus.subscribe<FileAnalyzeStartEvent>([](const FileAnalyzeStartEvent& e) {
        std::cout << "[ANALYZE] " << e.path << std::endl;
    });
    bus.subscribe<FileProcessCompleteEvent>([](const FileProcessCompleteEvent& e) {
        std::cout << "[DONE] " << e.path
                  << " (" << e.original_size << " -> " << e.new_size << " bytes)"
                  << (e.replaced ? " [replaced]" : " [kept]") << std::endl;
    });
    bus.subscribe<FileProcessErrorEvent>([](const FileProcessErrorEvent& e) {
        std::cerr << "[ERROR] " << e.path << ": " << e.error_message << std::endl;
    });
    bus.subscribe<ContainerFinalizeCompleteEvent>([](const ContainerFinalizeCompleteEvent& e) {
        std::cout << "[FINALIZED] " << e.path
                  << " (" << e.final_size << " bytes)" << std::endl;
    });

    // Costruisci executor
    ProcessorExecutor executor(registry,
                               settings.preserve_metadata,
                               settings.unencodable_target_format.value_or(ContainerFormat::Unknown),
                               settings.verify_checksums,
                               bus,
                               settings.num_threads);


    // ..
    auto inputs = collect_input_files(settings.inputs, settings.recursive, settings.is_pipe);
    if (inputs.empty()) {
        Logger::log(LogLevel::Error, "No valid input files.", "main");
        return 1;
    }
    executor.process(inputs);
    if (interrupted.load()) {
        return 130; // codice standard per SIGINT
    }
    return 0;
}