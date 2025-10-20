//
// Created by Giuseppe Francione on 20/10/25.
//

#ifndef CHISEL_EVENTS_HPP
#define CHISEL_EVENTS_HPP

#include <filesystem>
#include <string>
#include <chrono>

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
    std::filesystem::path path; ///< Path of the analyzed file
    bool extracted = false;     ///< True if the file was identified as a container and extracted
    bool scheduled = false;     ///< True if the file was scheduled for recompression
};

/**
 * @brief Emitted when analysis of a file fails.
 */
struct FileAnalyzeErrorEvent {
    std::filesystem::path path; ///< Path of the file
    std::string error_message;  ///< Error description
};

/**
 * @brief Emitted when a file is skipped during analysis.
 */
struct FileAnalyzeSkippedEvent {
    std::filesystem::path path; ///< Path of the skipped file
    std::string reason;         ///< Reason for skipping
};

// --- Phase 2: Processing ---

/**
 * @brief Emitted when processing of a file begins.
 */
struct FileProcessStartEvent {
    std::filesystem::path path; ///< Path of the file being processed
};

/**
 * @brief Emitted when processing of a file completes successfully.
 */
struct FileProcessCompleteEvent {
    std::filesystem::path path;        ///< Path of the processed file
    uintmax_t original_size = 0;       ///< Original file size in bytes
    uintmax_t new_size = 0;            ///< New file size in bytes
    bool replaced = false;             ///< True if the original file was replaced
    std::chrono::milliseconds duration{0}; ///< Processing duration
};

/**
 * @brief Emitted when processing of a file fails.
 */
struct FileProcessErrorEvent {
    std::filesystem::path path; ///< Path of the file
    std::string error_message;  ///< Error description
};

/**
 * @brief Emitted when a file is skipped during processing.
 */
struct FileProcessSkippedEvent {
    std::filesystem::path path; ///< Path of the skipped file
    std::string reason;         ///< Reason for skipping
};

// --- Phase 3: Finalization ---

/**
 * @brief Emitted when finalization of a container begins.
 */
struct ContainerFinalizeStartEvent {
    std::filesystem::path path; ///< Path of the container being finalized
};

/**
 * @brief Emitted when finalization of a container completes successfully.
 */
struct ContainerFinalizeCompleteEvent {
    std::filesystem::path path; ///< Path of the finalized container
    uintmax_t final_size = 0;   ///< Final size in bytes
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