//
// Created by Giuseppe Francione on 19/10/25.
//

/**
 * @file processor_executor.hpp
 * @brief Defines the main orchestrator for file processing.
 *
 * This file contains the ProcessorExecutor class, which manages
 * the entire lifecycle of file analysis, recompression, and
 * container finalization.
 */

#ifndef CHISEL_PROCESSOR_EXECUTOR_HPP
#define CHISEL_PROCESSOR_EXECUTOR_HPP

#include "processor.hpp"
#include "processor_registry.hpp"
#include <filesystem>
#include <vector>
#include <stack>
#include <optional>
#include <thread>
#include <mutex>
#include "event_bus.hpp"
#include "thread_pool.hpp"

namespace chisel {
    /**
     * @brief Defines the strategy for applying multiple processors to a single file.
     */
    enum class EncodeMode {
        /**
         * @brief Chain processors: output of one is input to the next.
         * (e.g., PngProcessor -> ZopfliPngProcessor)
         */
        PIPE,
        /**
         * @brief Run all processors on the original file and pick the smallest result.
         */
        PARALLEL                 [[deprecated("Use PIPE instead. PARALLEL mode is no longer recommended and will be removed in the future.")]]
    };
/**
 * @brief Orchestrates the analysis, processing, and finalization of files.
 *
 * @details ProcessorExecutor coordinates the three main phases of chisel:
 * - Phase 1: Recursive analysis of input files and containers.
 * - Phase 2: Recompression of eligible files (in PIPE or PARALLEL mode)
 * using the ThreadPool.
 * - Phase 3: Finalization (re-assembly) of containers after their
 * contents have been processed.
 *
 * It uses a ProcessorRegistry to discover processors, a ThreadPool to
 * parallelize work, and an EventBus to publish progress and results.
 */
class ProcessorExecutor {
public:
    /**
     * @brief Construct a ProcessorExecutor.
     *
     * @param registry Reference to a ProcessorRegistry with available processors.
     * @param options Processing options, such as metadata preservation, checksum verification and specific Processor settings.
     * @param mode Encoding mode (PIPE or PARALLEL).
     * @param dry_run If true, do not write or replace files.
     * @param output_dir If set, write optimized files here instead of in-place.
     * @param bus EventBus used to publish progress and results.
     * @param threads Number of worker threads to use.
     */
    explicit ProcessorExecutor(ProcessorRegistry& registry,
                               const ProcessingOptions &options,
                               EncodeMode mode,
                               bool dry_run,
                               std::filesystem::path output_dir,
                               EventBus& bus,
                               unsigned threads = std::thread::hardware_concurrency());

    /**
     * @brief Entry point: process a list of input files.
     *
     * This function executes the 3-phase processing pipeline.
     * @param inputs Vector of filesystem paths to process.
     */
    void process(const std::vector<std::filesystem::path>& inputs);

    std::optional<std::pair<std::filesystem::path, bool>> move_to_destination(
        const std::filesystem::path &original_file, const std::filesystem::path &temp_file) const;

    /**
     * @brief Checks if a stop has been requested.
     * @return true if the internal stop flag is set, false otherwise.
     * @note This reads the atomic stop flag with relaxed memory order.
     */
    [[nodiscard]] bool is_stopped() const {
        return stop_flag_.load(std::memory_order_relaxed);
    }

    /**
     * @brief Request the executor and its thread pool to stop.
     *
     * This is thread-safe and can be called from signal handlers
     * or other threads to request a graceful shutdown.
     */
    void request_stop();

private:
    /**
     * @brief Phase 1: Recursively analyze a path.
     *
     * If it's a file, it's added to work_list_.
     * If it's a container, its contents are extracted, added to
     * work_list_, and the container is added to finalize_stack_.
     *
     * @param path The file or directory path to analyze.
     * @param parent The parent container path if this is an extracted file.
     */
    void analyze_path(const std::filesystem::path& path, const std::optional<std::filesystem::path>& parent = std::nullopt);

    /**
     * @brief Phase 2: Recompress all files in work_list_ using the ThreadPool.
     *
     * Dispatches tasks according to the specified EncodeMode (PIPE or PARALLEL).
     */
    void process_work_list();

    /**
     * @brief Phase 3: Finalize all containers in finalize_stack_.
     *
     * This runs sequentially (LIFO) after all file processing is complete.
     */
    void finalize_containers();

    /**
     * @brief Handles file replacement logic after a task succeeds.
     *
     * Manages --dry-run, --output, and in-place replacement,
     * then publishes the final FileProcessCompleteEvent.
     *
     * @param original_file The path to the source file.
     * @param temp_file The path to the newly created optimized file.
     * @param original_size The size of the original file in bytes.
     * @param duration The time taken for the recompression task.
     */
    void handle_temp_file(const std::filesystem::path& original_file,
                            const std::filesystem::path& temp_file,
                            uintmax_t original_size,
                            std::chrono::milliseconds duration) const;
    ThreadPool pool_;                             ///< Thread pool for Phase 2
    ProcessingOptions m_options;
    std::stack<ExtractedContent> finalize_stack_; ///< (Phase 1->3) Containers to be re-assembled
    struct WorkItem {
        std::filesystem::path path;                            ///< Path to the file to be processed
        std::optional<std::filesystem::path> parent_container; ///< Path of the container this file was extracted from, if any
        bool is_container = false;                             ///< True if this item is the intermediate recompression of a container
    };
    std::vector<WorkItem> work_list_;///< (Phase 1->2) Files to be recompressed
    std::filesystem::path output_dir_;            ///< Optional output directory
    EventBus& event_bus_;                         ///< Bus for publishing events
    ProcessorRegistry& registry_;                 ///< Reference to the processor registry
    EncodeMode mode_;                             ///< (Phase 2) Strategy for recompression
    bool dry_run_;                                ///< If true, no files are written
    bool has_output_dir_;                         ///< Convenience flag for !output_dir_.empty()
    bool output_is_directory_ = true;             ///< True if the output path refers to a directory
    std::atomic<bool> stop_flag_{false};       ///< Flag to signal interruption

};

} // namespace chisel

#endif // CHISEL_PROCESSOR_EXECUTOR_HPP