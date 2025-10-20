//
// Created by Giuseppe Francione on 19/10/25.
//

#include "processor_executor.hpp"

#include "file_type.hpp"
#include "mime_detector.hpp"
#include "../utils/thread_pool.hpp"
#include "../utils/logger.hpp"

#include <filesystem>
#include <future>
#include <optional>
#include <vector>
#include <stack>
#include <string>

namespace chisel {

ProcessorExecutor::ProcessorExecutor(ProcessorRegistry& registry,
                            const bool preserve_metadata,
                            const ContainerFormat format,
                            const bool verify_checksums,
                            const unsigned threads):
    registry_(registry),
    preserve_metadata_(preserve_metadata),
    verify_checksums_(verify_checksums),
    format_(format),
    pool_(threads) {}

void ProcessorExecutor::process(const std::vector<std::filesystem::path>& inputs) {
    for (const auto& path : inputs) {
        analyze_path(path);
    }

    process_work_list();

    finalize_containers();
}

void ProcessorExecutor::analyze_path(const std::filesystem::path& path) {
    // tenta lookup via MIME
    auto mime = MimeDetector::detect(path);
    auto proc = registry_.find_by_mime(mime);

    // fallback su estensione se MIME non risolve
    if (!proc) {
        proc = registry_.find_by_extension(path.extension().string());
    }

    if (!proc) {
        Logger::log(LogLevel::Warning, "Nessun processor per " + path.string(), "Executor");
        return;
    }

    IProcessor* processor = *proc;

    if (processor->can_extract_contents()) {
        auto content = processor->prepare_extraction(path);
        if (content) {
            // pianifica la finalizzazione e analizza ricorsivamente i figli
            finalize_stack_.push(*content);
            for (const auto& child : content->extracted_files) {
                analyze_path(child);
            }
        } else {
            Logger::log(LogLevel::Error, "prepare_extraction ha fallito per " + path.string(), "Executor");
        }
    } else if (processor->can_recompress()) {
        work_list_.push_back(path);
    } else {
        Logger::log(LogLevel::Debug, "File ignorato: " + path.string(), "Executor");
    }
}

void ProcessorExecutor::process_work_list() {
    // invia ogni lavoro al thread pool
    for (const auto& file : work_list_) {
        pool_.enqueue([this, file](const std::stop_token& st) {
            // lookup processor preferendo extension (più stabile per singoli file)
            auto proc = registry_.find_by_extension(file.extension().string());
            if (!proc) {
                // fallback su MIME
                const auto mime = MimeDetector::detect(file);
                proc = registry_.find_by_mime(mime);
                if (!proc) {
                    Logger::log(LogLevel::Warning, "No processor for " + file.string(), "Executor");
                    return;
                }
            }


            std::filesystem::path tmp = file;
            tmp += ".tmp";

            try {
                IProcessor* processor = *proc;
                processor->recompress(file, tmp, verify_checksums_);

                std::error_code ec;
                auto orig_size = std::filesystem::file_size(file, ec);
                if (ec) orig_size = 0;
                auto new_size  = std::filesystem::file_size(tmp, ec);
                if (ec) new_size = 0;

                if (new_size > 0 && (orig_size == 0 || new_size < orig_size)) {
                    std::filesystem::rename(tmp, file, ec);
                    if (ec) {
                        Logger::log(LogLevel::Error, "Rename failed: " + file.string() + " (" + ec.message() + ")", "Executor");
                        // best-effort cleanup
                        std::filesystem::remove(tmp, ec);
                    } else {
                        Logger::log(LogLevel::Info, "Recompressed: " + file.string() +
                                                     " (" + std::to_string(orig_size) + " -> " +
                                                     std::to_string(new_size) + " bytes)", "Executor");
                    }
                } else {
                    std::filesystem::remove(tmp, ec);
                    Logger::log(LogLevel::Debug, "No improvement: " + file.string(), "Executor");
                }
            } catch (const std::exception& e) {
                Logger::log(LogLevel::Error, "Errore su " + file.string() + ": " + std::string(e.what()), "Executor");

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

        auto proc = registry_.find_by_extension(content.original_path.extension().string());
        if (!proc) {
            // fallback su MIME
            auto mime = MimeDetector::detect(content.original_path);
            proc = registry_.find_by_mime(mime);
            if (!proc) {
                Logger::log(LogLevel::Warning, "No processor to finalize: " + content.original_path.string(), "Executor");
                continue;
            }
        }

        try {
            // target_format: usa content.format quando disponibile
            (*proc)->finalize_extraction(content, format_);
        } catch (const std::exception& e) {
            Logger::log(LogLevel::Error,
                        "Finalize error: " + content.original_path.string() + " - " + std::string(e.what()),
                        "Executor");
        }
    }
}

} // namespace chisel