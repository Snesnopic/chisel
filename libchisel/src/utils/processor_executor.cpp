//
// Created by Giuseppe Francione on 19/10/25.
//

#include "../../include/processor_executor.hpp"
#include "../../include/file_type.hpp"
#include "../../include/mime_detector.hpp"
#include "../../include/thread_pool.hpp"
#include "../../include/logger.hpp"
#include "../../include/events.hpp"
#include "../../include/event_bus.hpp"
#include <filesystem>
#include <future>
#include <vector>
#include <stack>
#include <string>
#include <chrono>

namespace fs = std::filesystem;

namespace chisel {
    ProcessorExecutor::ProcessorExecutor(ProcessorRegistry &registry,
                                         const bool preserve_metadata,
                                         const ContainerFormat format,
                                         const bool verify_checksums,
                                         const EncodeMode mode,
                                         const bool dry_run,
                                         fs::path output_dir,
                                         EventBus &bus,
                                         const unsigned threads)
        : registry_(registry),
          preserve_metadata_(preserve_metadata),
          verify_checksums_(verify_checksums),
          dry_run_(dry_run),
          output_dir_(std::move(output_dir)),
          has_output_dir_(!output_dir_.empty()),
          format_(format),
          pool_(threads),
          event_bus_(bus),
          mode_(mode)
           {
                if (has_output_dir_ && !dry_run_) {
                    std::error_code ec;
                    fs::create_directories(output_dir_, ec);
                    if (ec) {
                        Logger::log(LogLevel::Error, "Failed to create output directory: " + output_dir_.string(), "Executor");
                        throw std::runtime_error("Failed to create output directory.");
                    }
                }
           }

    void ProcessorExecutor::process(const std::vector<fs::path> &inputs) {
        for (const auto &path: inputs) {
            if (stop_flag_.load(std::memory_order_relaxed)) return;
            analyze_path(path);
        }
        if (stop_flag_.load(std::memory_order_relaxed)) return;
        process_work_list();
        if (stop_flag_.load(std::memory_order_relaxed)) return;
        finalize_containers();
    }

    void ProcessorExecutor::handle_temp_file(const fs::path& original_file,
                                             const fs::path& temp_file,
                                             const uintmax_t original_size,
                                             const std::chrono::milliseconds duration) const {
        std::error_code ec;
        auto new_size = fs::file_size(temp_file, ec);
        if (ec || new_size == 0) {
            Logger::log(LogLevel::Warning, "Temp file is invalid or empty: " + temp_file.string(), "Executor");
            fs::remove(temp_file, ec);
            event_bus_.publish(FileProcessErrorEvent{original_file, "Failed to create optimized file"});
            return;
        }

        bool replaced = false;

        if (dry_run_) {
            Logger::log(LogLevel::Info, "[DRY-RUN] Would replace: " + original_file.string(), "Executor");
            fs::remove(temp_file, ec);

        } else if (has_output_dir_) {
            const fs::path dest = output_dir_ / original_file.filename();
            int retries = 10;
            while (retries > 0) {
                fs::rename(temp_file, dest, ec);
                if (!ec) break;

                if (ec.value() != 32 && ec.value() != 5 && ec.value() != 2) break;

                Logger::log(LogLevel::Debug, "Rename (output dir) failed (sharing violation), retrying in 250ms...", "Executor");
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                --retries;
            }
            if (ec) {
                const std::string rename_error = ec.message();
                Logger::log(LogLevel::Error, "Rename failed (in-place): " + original_file.string() + " (" + rename_error + ")", "Executor");
                fs::remove(temp_file, ec);
                event_bus_.publish(FileProcessErrorEvent{original_file, "Rename failed: " + rename_error});
                return;
            }
            replaced = true;

        } else { // in-place
            int retries = 10;
            while (retries > 0) {
                fs::rename(temp_file, original_file, ec);
                if (!ec) break; // success

                if (ec.value() != 32 && ec.value() != 5 && ec.value() != 2) break;

                Logger::log(LogLevel::Debug, "Rename failed (sharing/lock violation), retrying in 500ms...", "Executor");
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                --retries;
            }
            if (ec) {

                const std::string rename_error = ec.message();
                Logger::log(LogLevel::Error, "Rename failed (in-place): " + original_file.string() + " (" + rename_error + ")", "Executor");

                std::error_code remove_ec;
                fs::remove(temp_file, remove_ec);

                event_bus_.publish(FileProcessErrorEvent{original_file, "Rename failed: " + rename_error});
                return;
            }
            replaced = true;
        }

        event_bus_.publish(FileProcessCompleteEvent{original_file, original_size, new_size, replaced, duration});
    }


    void ProcessorExecutor::analyze_path(const fs::path &path) {
        if (stop_flag_.load(std::memory_order_relaxed)) return;

        auto name = path.filename().string();

        std::transform(name.begin(), name.end(), name.begin(), ::tolower);
        if (name == ".ds_store" || name == "desktop.ini" || name.starts_with("._")) {
            event_bus_.publish(FileAnalyzeSkippedEvent{path, "Junk file"});

            return;
        }

        event_bus_.publish(FileAnalyzeStartEvent{path});

        const auto mime = MimeDetector::detect(path);
        auto procs = registry_.find_by_mime(mime);
        if (procs.empty()) {
            procs = registry_.find_by_extension(path.extension().string());
        }

        if (procs.empty()) {
            Logger::log(LogLevel::Warning, "no processor for " + path.string(), "Executor");
            event_bus_.publish(FileAnalyzeSkippedEvent{path, "Unsupported format"});
            return;
        }

        IProcessor *processor = procs.front();

        const fs::path& current_path = path;
        bool scheduled_for_extraction = false;
        bool scheduled_for_recompression = false;
        std::optional<ExtractedContent> content;
        if (processor->can_extract_contents()) {
             content = processor->prepare_extraction(current_path);
            if (content) {
                finalize_stack_.push(*content);
                for (const auto &child: content->extracted_files) {
                    analyze_path(child);
                }
                scheduled_for_extraction = true;
            } else {
                if (processor->can_recompress()) {
                    Logger::log(LogLevel::Warning, "prepare_extraction resulted in no elements for " + path.string(), "Executor");
                    event_bus_.publish(FileAnalyzeSkippedEvent{path, "Extraction resulted in no elements"});
                }
                Logger::log(LogLevel::Error, "prepare_extraction failed for " + path.string(), "Executor");
                event_bus_.publish(FileAnalyzeErrorEvent{path, "Extraction failed"});
            }
        }
        if (processor->can_recompress()) {
            work_list_.push_back(current_path);
            scheduled_for_recompression = true;
        }
        if (scheduled_for_extraction || scheduled_for_recompression) {
            if (scheduled_for_extraction) {
                event_bus_.publish(FileAnalyzeCompleteEvent{path, true, scheduled_for_recompression, content->extracted_files.size()});
            } else {
                event_bus_.publish(FileAnalyzeCompleteEvent{path, false, scheduled_for_recompression});
            }
        } else {
            Logger::log(LogLevel::Debug, "file ignored: " + path.string(), "Executor");
            event_bus_.publish(FileAnalyzeSkippedEvent{path, "No operations available"});
        }
    }

    void ProcessorExecutor::process_work_list() {
        for (const auto &file: work_list_) {
            if (stop_flag_.load(std::memory_order_relaxed)) return;
            pool_.enqueue([this, file](const std::stop_token &st) {
                if (st.stop_requested()) {
                    event_bus_.publish(FileProcessSkippedEvent{file, "Interrupted"});
                    return;
                }
                event_bus_.publish(FileProcessStartEvent{file});

                // collect all candidates
                auto candidates = registry_.find_by_mime(MimeDetector::detect(file));
                if (candidates.empty()) {
                    candidates = registry_.find_by_extension(file.extension().string());
                }
                if (candidates.empty()) {
                    Logger::log(LogLevel::Warning, "no processor for " + file.string(), "Executor");
                    event_bus_.publish(FileProcessSkippedEvent{file, "Unsupported format"});
                    return;
                }

                auto safe_size = [](const fs::path &p) {
                    std::error_code ec;
                    const auto s = fs::file_size(p, ec);
                    return ec ? 0ull : s;
                };

                try {
                    const auto orig_size = safe_size(file);
                    auto start = std::chrono::steady_clock::now();

                    if (mode_ == EncodeMode::PIPE) {
                        fs::path current = file;
                        fs::path last_tmp;
                        bool pipeline_ok = true;

                        for (size_t i = 0; i < candidates.size(); ++i) {
                            if (st.stop_requested()) {
                                pipeline_ok = false;
                                break;
                            }

                            fs::path tmp = fs::temp_directory_path() / (file.filename().string() + ".pipe." + std::to_string(i) + ".tmp");

                            candidates[i]->recompress(current, tmp, preserve_metadata_);
                            auto sz = safe_size(tmp);
                            if (sz == 0) {
                                pipeline_ok = false;
                                std::error_code ec;
                                fs::remove(tmp, ec);
                                break;
                            }
                            if (current != file) {
                                std::error_code ec;
                                fs::remove(current, ec);
                            }
                            current = tmp;
                            last_tmp = tmp;
                        }

                        auto end = std::chrono::steady_clock::now();
                        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

                        if (pipeline_ok && !last_tmp.empty()) {
                            auto new_size = safe_size(last_tmp);
                            // accept the recompressed file only if it is smaller than the original
                            // and, if checksum verification is enabled, the raw checksums match
                            const bool size_improved = (new_size > 0 && new_size < orig_size);
                            const bool checksum_ok = !verify_checksums_ ||
                                candidates[0]->raw_equal(file, last_tmp);

                            if (size_improved && checksum_ok) {
                                handle_temp_file(file, last_tmp, orig_size, duration);
                            } else {
                                std::error_code ec;
                                fs::remove(last_tmp, ec);
                                event_bus_.publish(FileProcessSkippedEvent{file, "No size improvement"});
                            }
                        } else {
                            auto err = std::error_code{};
                            if (!last_tmp.empty()) fs::remove(last_tmp, err);
                            if (st.stop_requested()) {
                                event_bus_.publish(FileProcessSkippedEvent{file, "Interrupted"});
                            } else {
                                event_bus_.publish(FileProcessErrorEvent{file, "Pipeline failed"});
                            }
                        }
                    } else {
                        // parallel
                        struct Result {
                            fs::path tmp;
                            uintmax_t size{};
                            bool success{false};
                        };
                        std::vector<Result> results;

                        for (size_t i = 0; i < candidates.size(); ++i) {
                            if (st.stop_requested()) break;
                            fs::path tmp = fs::temp_directory_path() / (file.filename().string() + ".cand." + std::to_string(i) + ".tmp");
                            Result r{tmp, 0, false};
                            try {
                                candidates[i]->recompress(file, tmp, preserve_metadata_);
                                auto sz = safe_size(tmp);
                                if (sz > 0) {
                                    r.size = sz;
                                    r.success = true;
                                } else {
                                    std::error_code ec;
                                    fs::remove(tmp, ec);
                                }
                            } catch (...) {
                                std::error_code ec;
                                fs::remove(tmp, ec);
                            }
                            results.push_back(r);
                        }

                        auto end = std::chrono::steady_clock::now();
                        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

                        auto best_it = std::min_element(results.begin(), results.end(),
                                                        [](const Result &a, const Result &b) {
                                                            if (a.success != b.success) return a.success && !b.success;
                                                            return a.size < b.size;
                                                        });

                        if (best_it != results.end() && best_it->success && best_it->size < orig_size) {
                            handle_temp_file(file, best_it->tmp, orig_size, duration);
                            for (const auto &r: results) {
                                if (r.tmp != best_it->tmp) {
                                    std::error_code ec2;
                                    fs::remove(r.tmp, ec2);
                                }
                            }
                        } else {
                            for (const auto &r: results) {
                                std::error_code ec;
                                fs::remove(r.tmp, ec);
                            }
                            if (st.stop_requested()) {
                                event_bus_.publish(FileProcessSkippedEvent{file, "Interrupted"});
                            } else {
                                event_bus_.publish(FileProcessSkippedEvent{file, "No size improvement"});
                            }
                        }
                    }
                } catch (const std::exception &e) {
                    Logger::log(LogLevel::Error, "error on " + file.string() + ": " + std::string(e.what()),
                                "Executor");
                    event_bus_.publish(FileProcessErrorEvent{file, e.what()});
                }
            });
        }
        pool_.wait_idle();
    }

    void ProcessorExecutor::finalize_containers() {
        while (!finalize_stack_.empty() && !stop_flag_.load()) {
            auto content = finalize_stack_.top();
            finalize_stack_.pop();

            event_bus_.publish(ContainerFinalizeStartEvent{content.original_path});

            auto procs = registry_.find_by_mime(MimeDetector::detect(content.original_path));
            if (procs.empty()) {
                procs = registry_.find_by_extension(content.original_path.extension().string());
            }
            if (procs.empty()) {
                Logger::log(LogLevel::Warning, "no processor to finalize: " + content.original_path.string(),
                            "Executor");
                event_bus_.publish(ContainerFinalizeErrorEvent{content.original_path, "Unsupported format"});
                continue;
            }

            try {
                auto start = std::chrono::steady_clock::now();
                std::filesystem::path new_temp_file = procs.front()->finalize_extraction(content, format_);
                auto end = std::chrono::steady_clock::now();
                auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

                std::error_code ec;

                if (new_temp_file.empty()) {
                    Logger::log(LogLevel::Debug, "Container finalize skipped (no improvement): " + content.original_path.string(), "Executor");
                    const auto final_size = std::filesystem::file_size(content.original_path, ec);
                    event_bus_.publish(ContainerFinalizeCompleteEvent{content.original_path, ec ? 0 : final_size});
                    continue;
                }

                auto orig_size = std::filesystem::file_size(content.original_path, ec);
                if (ec) orig_size = 0;

                handle_temp_file(content.original_path, new_temp_file, orig_size, duration);

            } catch (const std::exception &e) {
                Logger::log(LogLevel::Error,
                            "Finalize error: " + content.original_path.string() + " - " + std::string(e.what()),
                            "Executor");
                event_bus_.publish(ContainerFinalizeErrorEvent{content.original_path, e.what()});
            }
        }
    }

    void ProcessorExecutor::request_stop() {
        stop_flag_.store(true, std::memory_order_relaxed);
        pool_.request_stop();
    }

} // namespace chisel
