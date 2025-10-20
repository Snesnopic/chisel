//
// Created by Giuseppe Francione on 20/10/25.
//

#ifndef CHISEL_EVENTS_HPP
#define CHISEL_EVENTS_HPP

#include <filesystem>
#include <string>
#include <chrono>

namespace chisel {

    // --- Phase 1: Analysis ---
    struct FileAnalyzeStartEvent {
        std::filesystem::path path;
    };

    struct FileAnalyzeCompleteEvent {
        std::filesystem::path path;
        bool extracted = false;   // true se è stato estratto un contenitore
        bool scheduled = false;   // true se è stato aggiunto alla work_list
    };

    struct FileAnalyzeErrorEvent {
        std::filesystem::path path;
        std::string error_message;
    };

    struct FileAnalyzeSkippedEvent {
        std::filesystem::path path;
        std::string reason;
    };

    // --- Phase 2: Processing ---
    struct FileProcessStartEvent {
        std::filesystem::path path;
    };

    struct FileProcessCompleteEvent {
        std::filesystem::path path;
        uintmax_t original_size = 0;
        uintmax_t new_size = 0;
        bool replaced = false;
        std::chrono::milliseconds duration{0};
    };

    struct FileProcessErrorEvent {
        std::filesystem::path path;
        std::string error_message;
    };

    struct FileProcessSkippedEvent {
        std::filesystem::path path;
        std::string reason;
    };

    // --- Phase 3: Finalization ---
    struct ContainerFinalizeStartEvent {
        std::filesystem::path path;
    };

    struct ContainerFinalizeCompleteEvent {
        std::filesystem::path path;
        uintmax_t final_size = 0;
    };

    struct ContainerFinalizeErrorEvent {
        std::filesystem::path path;
        std::string error_message;
    };

} // namespace chisel

#endif // CHISEL_EVENTS_HPP