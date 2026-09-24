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
#include <unordered_map>
#include <unordered_set>
#include <string>
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
     * This function executes the 3-phase processing pipeline. With an output path, it is the
     * output file when the only root is a file (unless it names a directory); otherwise it is a
     * directory where results keep their paths relative to the deepest folder holding all roots.
     * Inputs that aren't improved are copied there unchanged.
     * @param inputs Vector of filesystem paths to process.
     * @param roots Files and folders the inputs were collected from; defaults to the inputs.
     */
    void process(const std::vector<std::filesystem::path>& inputs,
                 const std::vector<std::filesystem::path>& roots = {});

    /**
     * @brief Moves an optimized temp file to its destination (in place, or into the output directory).
     * @param nested True for files extracted from a container: they are always replaced in place,
     *               since Phase 3 rebuilds their container from them.
     * @return The destination and whether the user-visible file was replaced, or std::nullopt on failure.
     */
    std::optional<std::pair<std::filesystem::path, bool>> move_to_destination(
        const std::filesystem::path &original_file, const std::filesystem::path &temp_file, bool nested);

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
     * @brief Maximum container nesting depth (archive-in-archive-in-archive...).
     *
     * Guards against maliciously crafted or accidental deeply-nested archives
     * exhausting the call stack or disk space (temp dirs accumulate until
     * Phase 3 finalization).
     */
    static constexpr unsigned kMaxNestingDepth = 256;

    /**
     * @brief Phase 1: Recursively analyze a path.
     *
     * If it's a file, it's added to work_list_.
     * If it's a container, its contents are extracted, added to
     * work_list_, and the container is added to finalize_stack_.
     *
     * @param path The file or directory path to analyze.
     * @param parent The parent container path if this is an extracted file.
     * @param depth Current container nesting depth (0 for top-level inputs).
     */
    void analyze_path(const std::filesystem::path& path, const std::optional<std::filesystem::path>& parent = std::nullopt, unsigned depth = 0);

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

    /// @brief Directory for a file's Phase 2 temp outputs, on the same mount point as their destination.
    [[nodiscard]] std::filesystem::path temp_dir_for(const std::filesystem::path& file, bool nested) const;

    /// @brief Where a top-level input's result goes under the output path.
    [[nodiscard]] std::filesystem::path destination_for(const std::filesystem::path& input) const;

    /**
     * @brief Dry run: keeps a container's Phase 2 result under its own file name, for Phase 3 to rebuild on.
     * @return The kept file, or std::nullopt on failure.
     */
    std::optional<std::filesystem::path> keep_for_finalize(const std::filesystem::path& file,
                                                           const std::filesystem::path& temp_file) const;

    /// @brief Copies the inputs that weren't written to the output path, unchanged.
    void copy_unchanged_inputs(const std::vector<std::filesystem::path>& inputs) const;

    ThreadPool pool_;                            ///< Thread pool for Phase 2
    ProcessingOptions m_options;
    struct PendingContainer {
        ExtractedContent content;
        bool nested = false; ///< True if the container was itself extracted from another container
    };
    std::stack<PendingContainer> finalize_stack_; ///< (Phase 1->3) Containers to be re-assembled
    struct WorkItem {
        std::filesystem::path path;                            ///< Path to the file to be processed
        std::optional<std::filesystem::path> parent_container; ///< Path of the container this file was extracted from, if any
        bool is_container = false;                             ///< True if this item is the intermediate recompression of a container
    };
    std::vector<WorkItem> work_list_;///< (Phase 1->2) Files to be recompressed

    /**
     * @brief (Phase 2->3) Actual on-disk location of each successfully recompressed
     * file, keyed by its pre-recompression path.
     *
     * For "mixed" processors (can_recompress() && can_extract_contents(), e.g.
     * FlacProcessor, ApeProcessor, OggProcessor, MkvProcessor, XmlProcessor),
     * Phase 3's finalize_extraction() rebuilds starting from ExtractedContent::original_path.
     * In the default in-place mode Phase 2 overwrites that same path, so this is a
     * no-op; but with --output-dir (or in a dry run, which keeps the result in
     * dry_run_dir_), Phase 2 writes the recompressed result to a different path
     * entirely, leaving original_path pointing at the untouched pristine file.
     * Without this map, Phase 3 would silently rebuild from that stale original,
     * discarding Phase 2's recompression outright.
     */
    std::unordered_map<std::string, std::filesystem::path> recompressed_paths_;
    std::unordered_set<std::string> outputs_written_; ///< Destinations already written under the output path
    mutable std::mutex recompressed_paths_mutex_; ///< Guards recompressed_paths_ and outputs_written_ (written concurrently by Phase 2's thread pool)

    std::filesystem::path output_dir_;            ///< Optional output directory
    std::filesystem::path output_base_;           ///< Folder the output directory mirrors
    std::filesystem::path dry_run_dir_;           ///< Kept Phase 2 results of a dry run, removed at the end
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