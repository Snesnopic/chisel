//
// Created by Giuseppe Francione on 19/10/25.
//

#include "processor_executor.hpp"

#include "file_type.hpp"
#include "mime_detector.hpp"
#include "../utils/thread_pool.hpp"
#include "../utils/logger.hpp"
#include "events.hpp"
#include "event_bus.hpp"

#include <filesystem>
#include <future>
#include <optional>
#include <vector>
#include <stack>
#include <string>
#include <chrono>

namespace chisel {

ProcessorExecutor::ProcessorExecutor(ProcessorRegistry& registry,
                                     const bool preserve_metadata,
                                     const ContainerFormat format,
                                     const bool verify_checksums,
                                     EventBus& bus,
                                     const unsigned threads)
    : registry_(registry),
      preserve_metadata_(preserve_metadata),
      verify_checksums_(verify_checksums),
      format_(format),
      pool_(threads),
      event_bus_(bus) {}

void ProcessorExecutor::process(const std::vector<std::filesystem::path>& inputs) {
    for (const auto& path : inputs) {
        analyze_path(path);
    }

    process_work_list();

    finalize_containers();
}

void ProcessorExecutor::analyze_path(const std::filesystem::path& path) {
    event_bus_.publish(FileAnalyzeStartEvent{path});

    auto mime = MimeDetector::detect(path);
    auto proc = registry_.find_by_mime(mime);

    if (!proc) {
        proc = registry_.find_by_extension(path.extension().string());
    }

    if (!proc) {
        Logger::log(LogLevel::Warning, "Nessun processor per " + path.string(), "Executor");
        event_bus_.publish(FileAnalyzeSkippedEvent{path, "Unsupported format"});
        return;
    }

    IProcessor* processor = *proc;

    if (processor->can_extract_contents()) {
        auto content = processor->prepare_extraction(path);
        if (content) {
            finalize_stack_.push(*content);
            for (const auto& child : content->extracted_files) {
                analyze_path(child);
            }
            event_bus_.publish(FileAnalyzeCompleteEvent{path, true, false});
        } else {
            Logger::log(LogLevel::Error, "prepare_extraction ha fallito per " + path.string(), "Executor");
            event_bus_.publish(FileAnalyzeErrorEvent{path, "Extraction failed"});
        }
    } else if (processor->can_recompress()) {
        work_list_.push_back(path);
        event_bus_.publish(FileAnalyzeCompleteEvent{path, false, true});
    } else {
        Logger::log(LogLevel::Debug, "File ignorato: " + path.string(), "Executor");
        event_bus_.publish(FileAnalyzeSkippedEvent{path, "No operations available"});
    }
}

void ProcessorExecutor::process_work_list() {
    for (const auto& file : work_list_) {
        pool_.enqueue([this, file](const std::stop_token& st) {
            event_bus_.publish(FileProcessStartEvent{file});

            auto proc = registry_.find_by_extension(file.extension().string());
            if (!proc) {
                const auto mime = MimeDetector::detect(file);
                proc = registry_.find_by_mime(mime);
                if (!proc) {
                    Logger::log(LogLevel::Warning, "No processor for " + file.string(), "Executor");
                    event_bus_.publish(FileProcessSkippedEvent{file, "Unsupported format"});
                    return;
                }
            }

            std::filesystem::path tmp = file;
            tmp += ".tmp";

            try {
                IProcessor* processor = *proc;
                auto start = std::chrono::steady_clock::now();

                processor->recompress(file, tmp, verify_checksums_);

                auto end = std::chrono::steady_clock::now();
                auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

                std::error_code ec;
                auto orig_size = std::filesystem::file_size(file, ec);
                if (ec) orig_size = 0;
                auto new_size  = std::filesystem::file_size(tmp, ec);
                if (ec) new_size = 0;

                bool replaced = false;
                if (new_size > 0 && (orig_size == 0 || new_size < orig_size)) {
                    std::filesystem::rename(tmp, file, ec);
                    if (ec) {
                        Logger::log(LogLevel::Error, "Rename failed: " + file.string() + " (" + ec.message() + ")", "Executor");
                        std::filesystem::remove(tmp, ec);
                        event_bus_.publish(FileProcessErrorEvent{file, "Rename failed: " + ec.message()});
                    } else {
                        replaced = true;
                        Logger::log(LogLevel::Info, "Recompressed: " + file.string() +
                                                     " (" + std::to_string(orig_size) + " -> " +
                                                     std::to_string(new_size) + " bytes)", "Executor");
                        event_bus_.publish(FileProcessCompleteEvent{file, orig_size, new_size, replaced, duration});
                    }
                } else {
                    std::filesystem::remove(tmp, ec);
                    Logger::log(LogLevel::Debug, "No improvement: " + file.string(), "Executor");
                    event_bus_.publish(FileProcessSkippedEvent{file, "No size improvement"});
                }
            } catch (const std::exception& e) {
                Logger::log(LogLevel::Error, "Errore su " + file.string() + ": " + std::string(e.what()), "Executor");
                event_bus_.publish(FileProcessErrorEvent{file, e.what()});

                std::error_code ec;
                tmp = file;
                tmp += ".tmp";
                std::filesystem::remove(tmp, ec);
            }
        });
    }

    pool_.wait_idle();
}

void ProcessorExecutor::finalize_containers() {
    while (!finalize_stack_.empty()) {
        auto content = finalize_stack_.top();
        finalize_stack_.pop();

        event_bus_.publish(ContainerFinalizeStartEvent{content.original_path});

        auto proc = registry_.find_by_extension(content.original_path.extension().string());
        if (!proc) {
            auto mime = MimeDetector::detect(content.original_path);
            proc = registry_.find_by_mime(mime);
            if (!proc) {
                Logger::log(LogLevel::Warning, "No processor to finalize: " + content.original_path.string(), "Executor");
                event_bus_.publish(ContainerFinalizeErrorEvent{content.original_path, "Unsupported format"});
                continue;
            }
        }

        try {
            (*proc)->finalize_extraction(content, format_);

            std::error_code ec;
            auto final_size = std::filesystem::file_size(content.original_path, ec);
            if (ec) final_size = 0;

            event_bus_.publish(ContainerFinalizeCompleteEvent{content.original_path, final_size});
        } catch (const std::exception& e) {
            Logger::log(LogLevel::Error,
                        "Finalize error: " + content.original_path.string() + " - " + std::string(e.what()),
                        "Executor");
            event_bus_.publish(ContainerFinalizeErrorEvent{content.original_path, e.what()});
        }
    }
}

} // namespace chisel