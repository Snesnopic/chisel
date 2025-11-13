//
// Created by Giuseppe Francione on 19/10/25.
//

#ifndef CHISEL_PROCESSOR_EXECUTOR_HPP
#define CHISEL_PROCESSOR_EXECUTOR_HPP

#include "processor.hpp"
#include "processor_registry.hpp"
#include <filesystem>
#include <vector>
#include <stack>
#include <thread>
#include <mutex>
#include "event_bus.hpp"
#include "thread_pool.hpp"

enum class EncodeMode {
    PIPE,      // one encoder output is the next one's output
    PARALLEL   // all encoders on the original file
};

namespace chisel {

/**
 * @brief Orchestrates the analysis, processing, and finalization of files.
 *
 * ProcessorExecutor coordinates the three main phases of chisel:
 *  - Phase 1: Recursive analysis of input files and containers
 *  - Phase 2: Recompression of eligible files (PIPE or PARALLEL mode)
 *  - Phase 3: Finalization of containers after their contents are processed
 *
 * It uses a ProcessorRegistry to discover processors, a ThreadPool to
 * parallelize work across files, and an EventBus to publish progress
 * and results to interested listeners (e.g. CLI, GUI, report generator).
 */
class ProcessorExecutor {
public:
    /**
     * @brief Construct a ProcessorExecutor.
     * @param registry Reference to a ProcessorRegistry with available processors.
     * @param preserve_metadata Whether to preserve metadata during recompression.
     * @param format Target container format for unencodable archives.
     * @param verify_checksums Whether to verify checksums after recompression.
     * @param mode Encoding mode (PIPE or PARALLEL).
     * @param dry_run If true, do not write files.
     * @param output_dir If set, write files to this directory.
     * @param bus EventBus used to publish progress and results.
     * @param threads Number of worker threads to use (default: hardware concurrency).
     */
    explicit ProcessorExecutor(ProcessorRegistry& registry,
                               bool preserve_metadata,
                               ContainerFormat format,
                               bool verify_checksums,
                               EncodeMode mode,
                               bool dry_run,
                               std::filesystem::path output_dir,
                               EventBus& bus,
                               unsigned threads = std::thread::hardware_concurrency());

    /**
     * @brief Entry point: process a list of input files.
     * @param inputs Vector of filesystem paths to process.
     */
    void process(const std::vector<std::filesystem::path>& inputs);

    /**
     * @brief Checks if the executor was requested to stop.
     * @return true if request_stop() was called, false otherwise.
     */
    [[nodiscard]] bool is_stopped() const {
        return stop_flag_.load(std::memory_order_relaxed);
    }

    void request_stop();
private:
    void check_for_stop_request();
    /// Phase 1: recursively analyze files and containers
    void analyze_path(const std::filesystem::path& path);

    /// Phase 2: recompress files in work_list_ using PIPE or PARALLEL mode
    void process_work_list();

    /// Phase 3: finalize containers after their contents have been processed
    void finalize_containers();

    void handle_temp_file(const std::filesystem::path& original_file,
                            const std::filesystem::path& temp_file,
                            uintmax_t original_size,
                            std::chrono::milliseconds duration) const;

    ProcessorRegistry& registry_;                 ///< Processor registry reference
    bool preserve_metadata_;                      ///< Preserve metadata flag
    bool verify_checksums_;                       ///< Verify checksums flag
    bool dry_run_;                                ///< Dry run flag
    std::filesystem::path output_dir_;            ///< Output directory
    bool has_output_dir_;                         ///< Convenience flag
    ContainerFormat format_;                      ///< Target format for unencodable containers
    std::vector<std::filesystem::path> work_list_;///< Files scheduled for recompression
    std::stack<ExtractedContent> finalize_stack_; ///< Stack of containers to finalize
    std::mutex log_mutex_;                        ///< Protects logging
    ThreadPool pool_;                             ///< Thread pool for parallel execution
    std::atomic<bool> stop_flag_{false};       ///< Flag to signal stop
    EventBus& event_bus_;                         ///< Event bus for publishing events
    EncodeMode mode_;                             ///< Encoding mode (PIPE or PARALLEL)
};

} // namespace chisel

#endif // CHISEL_PROCESSOR_EXECUTOR_HPP