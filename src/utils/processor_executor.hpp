//
// Created by Giuseppe Francione on 19/10/25.
//

#ifndef CHISEL_PROCESSOR_EXECUTOR_HPP
#define CHISEL_PROCESSOR_EXECUTOR_HPP

#include "../processors/processor.hpp"
#include "processor_registry.hpp"
#include "../utils/logger.hpp"

#include <filesystem>
#include <vector>
#include <stack>
#include <optional>
#include <future>
#include <thread>
#include <mutex>

#include "event_bus.hpp"
#include "thread_pool.hpp"

namespace chisel {

    struct ProcessingOptions {
        bool verify_checksums = false;
        bool keep_metadata = true;
    };

    class ProcessorExecutor {
    public:
        explicit ProcessorExecutor(ProcessorRegistry& registry,
                                     bool preserve_metadata,
                                     ContainerFormat format,
                                     bool verify_checksums,
                                     EventBus& bus,
                                     unsigned threads = std::thread::hardware_concurrency());


        // entry point: processa una lista di file
        void process(const std::vector<std::filesystem::path>& inputs);

    private:
        // --- Phase 1: analisi ricorsiva ---
        void analyze_path(const std::filesystem::path& path);

        // --- Phase 2: parallel recompression ---
        void process_work_list();

        // --- Phase 3: finalizzazione ricorsiva ---
        void finalize_containers();

        ProcessorRegistry& registry_;
        bool preserve_metadata_;
        bool verify_checksums_;

        ContainerFormat format_;
        std::vector<std::filesystem::path> work_list_;
        std::stack<ExtractedContent> finalize_stack_;

        std::mutex log_mutex_;
        ThreadPool pool_;
        EventBus& event_bus_;
    };

} // namespace chisel

#endif // CHISEL_PROCESSOR_EXECUTOR_HPP