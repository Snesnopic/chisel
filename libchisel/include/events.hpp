//
// Created by Giuseppe Francione on 20/10/25.
//

/**
 * @file events.hpp
 * @brief Defines all event data structures used by the EventBus.
 *
 * These lightweight structs are used with EventBus to notify subscribers
 * about progress, errors, and results during processing.
 */

#ifndef CHISEL_EVENTS_HPP
#define CHISEL_EVENTS_HPP

#include <filesystem>
#include <string>
#include <chrono>
#include <optional>

namespace chisel {

/**
 * @brief Events published during the three main phases of processing.
 *
 * These lightweight structs are used with EventBus to notify subscribers
 * (e.g. CLI, report generator, GUI) about progress, errors, and results.
 * They are simple data carriers without behavior.
 */

// --- Phase 1: Analysis ---

/**
 * @brief Emitted when analysis of a file begins.
 */
struct FileAnalyzeStartEvent {
    std::filesystem::path path; ///< Path of the file being analyzed
};

/**
 * @brief Emitted when analysis of a file completes.
 */
struct FileAnalyzeCompleteEvent {
    std::filesystem::path path;     ///< Path of the analyzed file
    bool extracted = false;         ///< True if the file was a container and extracted
    bool scheduled = false;         ///< True if the file was scheduled for recompression
    std::size_t num_children = 0;   ///< Number of files found if extracted
    unsigned depth = 0;              ///< Nesting depth (0 = direct input, >0 = found inside another container)
};

/**
 * @brief Emitted when analysis of a file fails.
 */
struct FileAnalyzeErrorEvent {
    std::filesystem::path path; ///< Path of the file
    std::string error_message;  ///< Error description
};

/**
 * @brief Emitted when a file is skipped during analysis (e.g., junk file, unsupported).
 */
struct FileAnalyzeSkippedEvent {
    std::filesystem::path path; ///< Path of the skipped file
    std::string reason;         ///< Reason for skipping
};

// --- Phase 2: Processing ---

/**
 * @brief Emitted when recompression of a file begins.
 */
struct FileProcessStartEvent {
    std::filesystem::path path; ///< Path of the file being processed
    std::optional<std::filesystem::path> parent_container = std::nullopt; ///< Path of the container this file was extracted from, if any
    bool is_container = false;                                            ///< True if this event refers to the intermediate recompression of a container
};

/**
 * @brief Emitted when processing of a file completes successfully.
 */
struct FileProcessCompleteEvent {
    std::filesystem::path path;             ///< Path of the processed file
    std::filesystem::path destination;      ///< Final path where the file was saved
    uintmax_t original_size = 0;            ///< Original file size in bytes
    uintmax_t new_size = 0;                 ///< New file size in bytes
    bool replaced = false;                  ///< True if the original file was replaced/written
    std::chrono::milliseconds duration{0};  ///< Processing duration
    std::optional<std::filesystem::path> parent_container = std::nullopt; ///< Path of the container this file was extracted from, if any
    bool is_container = false;                                            ///< True if this event refers to the intermediate recompression of a container
};

/**
 * @brief Emitted when processing of a file fails with an exception.
 */
struct FileProcessErrorEvent {
    std::filesystem::path path; ///< Path of the file
    std::string error_message;                                            ///< Error description
    bool is_container = false;                                            ///< True if this event refers to the intermediate recompression of a container
};

/**
 * @brief Emitted when a file is skipped during processing (e.g., no size improvement).
 */
struct FileProcessSkippedEvent {
    std::filesystem::path path; ///< Path of the skipped file
    std::string reason;                                                   ///< Reason for skipping
    bool is_container = false;                                            ///< True if this event refers to the intermediate recompression of a container
};

// --- Phase 3: Finalization ---

/**
 * @brief Emitted when finalization (re-assembly) of a container begins.
 */
struct ContainerFinalizeStartEvent {
    std::filesystem::path path; ///< Path of the container being finalized
};

/**
 * @brief Emitted when finalization of a container completes successfully.
 */
struct ContainerFinalizeCompleteEvent {
    std::filesystem::path path;             ///< path of the container (input)
    std::filesystem::path destination;      ///< final path of the container
    uintmax_t original_size = 0;
    uintmax_t final_size = 0;
    bool replaced = false;                  ///< consistency with file events
    std::chrono::milliseconds duration{0};///< added for reporting
};

/**
 * @brief Emitted when finalization of a container fails.
 */
struct ContainerFinalizeErrorEvent {
    std::filesystem::path path; ///< Path of the container
    std::string error_message;  ///< Error description
};

} // namespace chisel

#endif // CHISEL_EVENTS_HPP