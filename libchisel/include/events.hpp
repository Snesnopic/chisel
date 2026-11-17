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

/**
 * @brief Why a file was left as it is.
 */
enum class SkipReason {
    Junk,           ///< System or metadata file, e.g. .DS_Store or Thumbs.db
    Empty,          ///< Zero bytes long
    Unsupported,    ///< No processor handles its format
    Signed,         ///< Left untouched to keep its digital signature valid
    NestingTooDeep, ///< Found inside more containers than allowed
    NothingToDo,    ///< Its processor can't recompress it and found nothing inside it
    NoImprovement,  ///< Recompressing it gave nothing smaller
    Interrupted     ///< A stop was requested before it was done
};

// --- Phase 1: Analysis ---

/**
 * @brief Emitted when analysis of a file begins.
 */
struct FileAnalyzeStartEvent {
    std::filesystem::path path; ///< Path of the file being analyzed
    std::optional<std::filesystem::path> parent_container; ///< Container the file was extracted from, if any
};

/**
 * @brief Emitted when analysis of a file completes.
 */
struct FileAnalyzeCompleteEvent {
    std::filesystem::path path;     ///< Path of the analyzed file
    std::optional<std::filesystem::path> parent_container; ///< Container the file was extracted from, if any
    bool extracted = false;         ///< True if the file was a container and extracted
    bool scheduled = false;         ///< True if the file was scheduled for recompression
    std::size_t num_children = 0;   ///< Number of files found if extracted
    unsigned depth = 0;              ///< Nesting depth (0 = direct input, >0 = found inside another container)
    std::filesystem::path root;      ///< Top-level input the file was found in
};

/**
 * @brief Emitted when a top-level input and everything found inside it has been analyzed.
 *
 * Inputs are analyzed in parallel, so this is where a consumer can present one input's findings together.
 */
struct InputAnalyzeCompleteEvent {
    std::filesystem::path path; ///< The top-level input
};

/**
 * @brief Emitted when analysis of a file fails.
 */
struct FileAnalyzeErrorEvent {
    std::filesystem::path path; ///< Path of the file
    std::optional<std::filesystem::path> parent_container; ///< Container the file was extracted from, if any
    std::string error_message;  ///< Error description
};

/**
 * @brief Emitted when a file is skipped during analysis (e.g., junk file, unsupported).
 */
struct FileAnalyzeSkippedEvent {
    std::filesystem::path path; ///< Path of the skipped file
    std::optional<std::filesystem::path> parent_container; ///< Container the file was extracted from, if any
    SkipReason reason;          ///< Why it was skipped
    std::string message;        ///< The reason as text, for display
};

// --- Phase 2: Processing ---

/**
 * @brief Emitted when recompression of a file begins.
 */
struct FileProcessStartEvent {
    std::filesystem::path path; ///< Path of the file being processed
    std::optional<std::filesystem::path> parent_container; ///< Container the file was extracted from, if any
    bool is_container = false;                                            ///< True if this event refers to the intermediate recompression of a container
};

/**
 * @brief Emitted when processing of a file completes successfully.
 */
struct FileProcessCompleteEvent {
    std::filesystem::path path;             ///< Path of the processed file
    std::optional<std::filesystem::path> parent_container; ///< Container the file was extracted from, if any
    std::filesystem::path destination;      ///< Final path where the file was saved
    uintmax_t original_size = 0;            ///< Original file size in bytes
    uintmax_t new_size = 0;                 ///< New file size in bytes
    bool replaced = false;                  ///< True if the original file was replaced/written
    std::chrono::milliseconds duration{0};  ///< Processing duration
    bool is_container = false;                                            ///< True if this event refers to the intermediate recompression of a container
};

/**
 * @brief Emitted when processing of a file fails with an exception.
 */
struct FileProcessErrorEvent {
    std::filesystem::path path; ///< Path of the file
    std::optional<std::filesystem::path> parent_container; ///< Container the file was extracted from, if any
    std::string error_message;                                            ///< Error description
    bool is_container = false;                                            ///< True if this event refers to the intermediate recompression of a container
};

/**
 * @brief Emitted when a file is skipped during processing (e.g., no size improvement).
 */
struct FileProcessSkippedEvent {
    std::filesystem::path path; ///< Path of the skipped file
    std::optional<std::filesystem::path> parent_container; ///< Container the file was extracted from, if any
    SkipReason reason;          ///< Why it was skipped
    std::string message;        ///< The reason as text, for display
    bool is_container = false;                                            ///< True if this event refers to the intermediate recompression of a container
};

// --- Phase 3: Finalization ---

/**
 * @brief Emitted when finalization (re-assembly) of a container begins.
 */
struct ContainerFinalizeStartEvent {
    std::filesystem::path path; ///< Path of the container being finalized
    std::optional<std::filesystem::path> parent_container; ///< Container the file was extracted from, if any
};

/**
 * @brief Emitted when finalization of a container completes successfully.
 */
struct ContainerFinalizeCompleteEvent {
    std::filesystem::path path;             ///< path of the container (input)
    std::optional<std::filesystem::path> parent_container; ///< Container the file was extracted from, if any
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
    std::optional<std::filesystem::path> parent_container; ///< Container the file was extracted from, if any
    std::string error_message;  ///< Error description
};

/**
 * @brief Emitted when a container is left as it was without being rebuilt, e.g. because a stop was requested.
 *
 * Every container that was extracted ends with exactly one of ContainerFinalizeCompleteEvent,
 * ContainerFinalizeErrorEvent or this event.
 */
struct ContainerFinalizeSkippedEvent {
    std::filesystem::path path; ///< Path of the container
    std::optional<std::filesystem::path> parent_container; ///< Container it was extracted from, if any
    SkipReason reason;          ///< Why it wasn't rebuilt
    std::string message;        ///< The reason as text, for display
};

} // namespace chisel

#endif // CHISEL_EVENTS_HPP