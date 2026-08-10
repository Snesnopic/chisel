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
#include <fstream>
#include "random_utils.hpp"


namespace fs = std::filesystem;

namespace chisel {
    ProcessorExecutor::ProcessorExecutor(ProcessorRegistry &registry,
                                         const ProcessingOptions &options,
                                         const EncodeMode mode,
                                         const bool dry_run,
                                         fs::path output_dir,
                                         EventBus &bus,
                                         const unsigned threads)
        : pool_(threads),
          m_options(options),
          output_dir_(std::move(output_dir)),
          event_bus_(bus),
          registry_(registry),
          mode_(mode),
          dry_run_(dry_run),
          has_output_dir_(!output_dir_.empty())
           {
           }

    void ProcessorExecutor::process(const std::vector<fs::path> &inputs) {
        if (has_output_dir_ && !dry_run_) {
            bool create_dir = false;
            if (inputs.size() > 1) {
                // Multiple inputs -> Output must be a directory
                output_is_directory_ = true;
                create_dir = true;
            } else {
                // Single input
                if (fs::is_directory(output_dir_)) {
                    // Output exists and is a directory
                    output_is_directory_ = true;
                } else {
                    // Output does not exist OR is a file -> Treat as filename
                    output_is_directory_ = false;

                    // Create parent directory if needed?
                    // Standard cp fails if parent doesn't exist, but we can be nice.
                    if (output_dir_.has_parent_path()) {
                        std::error_code ec;
                        fs::create_directories(output_dir_.parent_path(), ec);
                    }
                }
            }

            if (create_dir) {
                std::error_code ec;
                fs::create_directories(output_dir_, ec);
                if (ec) {
                    Logger::log(LogLevel::Error, "Failed to create output directory: " + output_dir_.string(), "Executor");
                    return; // Abort if we can't create output dir
                }
            }
        }

        for (const auto &path: inputs) {
            if (stop_flag_.load(std::memory_order_relaxed)) return;
            analyze_path(path);
        }
        if (stop_flag_.load(std::memory_order_relaxed)) return;
        process_work_list();
        if (stop_flag_.load(std::memory_order_relaxed)) return;
        finalize_containers();
    }

    std::optional<std::pair<fs::path, bool>> ProcessorExecutor::move_to_destination(
        const fs::path& original_file,
        const fs::path& temp_file) const {

        std::error_code ec;
        const auto new_size = fs::file_size(temp_file, ec);
        if (ec || new_size == 0) {
            Logger::log(LogLevel::Warning, "Temp file is invalid or empty: " + temp_file.string(), "Executor");
            fs::remove(temp_file, ec);
            return std::nullopt;
        }

        bool replaced = false;
        fs::path dest = original_file;

        if (dry_run_) {
            Logger::log(LogLevel::Info, "[DRY-RUN] Would replace: " + original_file.string(), "Executor");
            fs::remove(temp_file, ec);
        } else if (has_output_dir_) {
            dest = output_is_directory_
                  ? (output_dir_ / original_file.filename())
                  : output_dir_;

            int retries = 10;
            while (retries > 0) {
                fs::rename(temp_file, dest, ec);
                if (ec == std::errc::cross_device_link) {
                    ec.clear();
                    fs::copy(temp_file, dest, fs::copy_options::overwrite_existing, ec);
                    if (!ec) fs::remove(temp_file, ec);
                }
                if (!ec) {
                    replaced = true;
                    break;
                }
#ifdef _WIN32
                if (ec.value() != 32 && ec.value() != 5 && ec.value() != 2) break;
#else
                // posix specific retry conditions or generic fallback
                if (ec.value() != EACCES && ec.value() != ETXTBSY) break;
#endif

                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                --retries;
            }
            if (ec) {
                Logger::log(LogLevel::Error, "Rename failed: " + dest.string() + " (" + ec.message() + ")", "Executor");
                fs::remove(temp_file, ec);
                return std::nullopt;
            }
            replaced = true;
        } else {
            // in-place
            int retries = 10;
            while (retries > 0) {
                fs::rename(temp_file, original_file, ec);

#ifdef __APPLE__
                // fallback to stream copy for sandboxed environments
                if (ec) {
                    ec.clear();
                    std::ifstream src(temp_file, std::ios::binary);
                    std::ofstream dst(original_file, std::ios::binary | std::ios::trunc);

                    if (src && dst) {
                        dst << src.rdbuf();
                        dst.flush();

                        if (dst.good()) {
                            Logger::log(LogLevel::Info, "STREAM OVERWRITE SUCCESSFUL", "Executor");
                            fs::remove(temp_file, ec);
                            ec.clear();
                        } else {
                            Logger::log(LogLevel::Error, "FAILED TO FLUSH STREAM", "Executor");
                            ec = std::make_error_code(std::errc::io_error);
                        }
                    } else {
                        ec = std::make_error_code(std::errc::permission_denied);
                    }
                }
#else
                if (ec == std::errc::cross_device_link) {
                    ec.clear();
                    fs::copy(temp_file, original_file, fs::copy_options::overwrite_existing, ec);
                    if (!ec) fs::remove(temp_file, ec);
                }
#endif

                if (!ec) {
                    replaced = true;
                    break;
                }
#ifdef _WIN32
                if (ec.value() != 32 && ec.value() != 5 && ec.value() != 2) break;
#else
                // posix specific retry conditions or generic fallback
                if (ec.value() != EACCES && ec.value() != ETXTBSY) break;
#endif

                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                --retries;
            }
        }

        return std::make_pair(dest, replaced);
    }

    void ProcessorExecutor::analyze_path(const fs::path &path, const std::optional<fs::path>& parent, const unsigned depth) {
        if (stop_flag_.load(std::memory_order_relaxed)) return;

        if (depth > kMaxNestingDepth) {
            Logger::log(LogLevel::Error,
                        "Maximum container nesting depth (" + std::to_string(kMaxNestingDepth) +
                        ") exceeded, refusing to descend further into: " + path.string(),
                        "Executor");
            event_bus_.publish(FileAnalyzeSkippedEvent{path, "Maximum nesting depth exceeded"});
            return;
        }

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
            Logger::log(LogLevel::Warning, "No processor for " + path.string(), "Executor");
            event_bus_.publish(FileAnalyzeSkippedEvent{path, "Unsupported format"});
            return;
        }

        IProcessor *processor = procs.front();

        const fs::path& current_path = path;
        bool scheduled_for_extraction = false;
        bool scheduled_for_recompression = false;
        std::optional<ExtractedContent> content;
        if (processor->can_extract_contents()) {
            try {
                content = processor->prepare_extraction(current_path);
            } catch (const std::exception& e) {
                Logger::log(LogLevel::Error, "Exception during prepare_extraction for " + path.string() + ": " + e.what(), "Executor");
                content = std::nullopt;
            } catch (...) {
                Logger::log(LogLevel::Error, "Unknown exception during prepare_extraction for " + path.string(), "Executor");
                content = std::nullopt;
            }
            if (content) {
                std::error_code ec;
                content->original_size = fs::file_size(content->original_path, ec);
                if (ec) content->original_size = 0;
                finalize_stack_.push(*content);
                for (const auto &child: content->extracted_files) {
                    analyze_path(child, path, depth + 1);
                }
                scheduled_for_extraction = true;
            } else {
                if (processor->can_recompress()) {
                    Logger::log(LogLevel::Warning, "Prepare_extraction resulted in no elements for " + path.string(), "Executor");
                    event_bus_.publish(FileAnalyzeSkippedEvent{path, "Extraction resulted in no elements"});
                } else {
                    Logger::log(LogLevel::Warning, "Prepare_extraction skipped or resulted in no elements for " + path.string(), "Executor");
                    event_bus_.publish(FileAnalyzeErrorEvent{path, "Extraction failed or skipped"});
                }
            }
        }
        if (processor->can_recompress()) {
            work_list_.push_back({current_path, parent, scheduled_for_extraction});
            scheduled_for_recompression = true;
        }
        if (scheduled_for_extraction || scheduled_for_recompression) {
            if (scheduled_for_extraction) {
                event_bus_.publish(FileAnalyzeCompleteEvent{path, true, scheduled_for_recompression, content->extracted_files.size(), depth});
            } else {
                event_bus_.publish(FileAnalyzeCompleteEvent{path, false, scheduled_for_recompression, 0, depth});
            }
        } else {
            Logger::log(LogLevel::Debug, "File ignored: " + path.string(), "Executor");
            event_bus_.publish(FileAnalyzeSkippedEvent{path, "No operations available"});
        }
    }

    void ProcessorExecutor::process_work_list() {
        for (const auto &item: work_list_) {
            if (stop_flag_.load(std::memory_order_relaxed)) return;
            pool_.enqueue([this, item](stop_token st) {
                const auto& file = item.path;
                const auto& parent_container = item.parent_container;
                if (st.stop_requested()) {
                    event_bus_.publish(FileProcessSkippedEvent{file, "Interrupted", item.is_container});
                    return;
                }
                event_bus_.publish(FileProcessStartEvent{file, parent_container, item.is_container});

                // collect all candidates
                auto candidates = registry_.find_by_mime(MimeDetector::detect(file));
                if (candidates.empty()) {
                    candidates = registry_.find_by_extension(file.extension().string());
                }
                if (candidates.empty()) {
                    Logger::log(LogLevel::Warning, "No processor for " + file.string(), "Executor");
                    event_bus_.publish(FileAnalyzeSkippedEvent{file, "Unsupported format"});
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
                    const std::string job_suffix = RandomUtils::random_suffix();

                    fs::path final_temp_path;
                    uintmax_t new_size = 0;
                    bool success = false;

                    if (mode_ == EncodeMode::PIPE) {
                        fs::path current = file;
                        fs::path last_tmp;
                        bool pipeline_ok = true;
                        struct LastTmpGuard {
                            fs::path& path;
                            bool release = false;
                            ~LastTmpGuard() {
                                if (!release && !path.empty()) {
                                    std::error_code ec;
                                    fs::remove(path, ec);
                                }
                            }
                        } last_tmp_guard{last_tmp, false};

                        for (std::size_t i = 0; i < candidates.size(); ++i) {
                            if (st.stop_requested()) {
                                pipeline_ok = false;
                                break;
                            }
#ifdef __APPLE__
                            fs::path target_dir = has_output_dir_
                                ? (output_is_directory_ ? output_dir_ : output_dir_.parent_path())
                                : fs::temp_directory_path(); // use system temp to bypass sandbox restrictions
#else
                            // resolve target directory to keep temp file on the same mount point
                            fs::path target_dir = has_output_dir_
                                ? (output_is_directory_ ? output_dir_ : output_dir_.parent_path())
                                : file.parent_path();
#endif

                            fs::path tmp = target_dir / (file.filename().string() + "_" + job_suffix + ".pipe." + std::to_string(i) + ".tmp");
                            struct TempFileGuard {
                                fs::path path;
                                bool release = false;
                                ~TempFileGuard() {
                                    if (!release && !path.empty()) {
                                        std::error_code ec;
                                        fs::remove(path, ec);
                                    }
                                }
                            } tmp_guard{tmp, false};

                            candidates[i]->recompress(current, tmp, m_options);
                            auto sz = safe_size(tmp);
                            if (sz == 0) {
                                pipeline_ok = false;
                                break;
                            }
                            tmp_guard.release = true; // file is good, don't delete yet
                            if (current != file) {
                                std::error_code ec;
                                fs::remove(current, ec);
                            }
                            current = tmp;
                            last_tmp = tmp;
                        }

                        if (pipeline_ok && !last_tmp.empty()) {
                            new_size = safe_size(last_tmp);
                            // accept the recompressed file only if it is smaller than the original
                            // and, if checksum verification is enabled, the raw checksums match
                            const bool size_improved = (new_size > 0 && new_size < orig_size);
                            const bool checksum_ok = !m_options.verify_checksums ||
                                candidates[0]->raw_equal(file, last_tmp);

                            if (size_improved && checksum_ok) {
                                final_temp_path = last_tmp;
                                success = true;
                                last_tmp_guard.release = true; // ownership transferred to final_temp_path
                            } else {
                                std::error_code ec;
                                fs::remove(last_tmp, ec);
                                if (!checksum_ok) {
                                    event_bus_.publish(FileProcessErrorEvent{file, "INTEGRITY CHECK FAILED: Data corruption detected", item.is_container});
                                } else {
                                    Logger::log(LogLevel::Debug, "No size improvement, keeping original: " + file.string(), "Executor");
                                    event_bus_.publish(FileProcessSkippedEvent{file, "No size improvement", item.is_container});
                                }
                            }
                        } else if (!st.stop_requested()) {
                            auto err = std::error_code{};
                            if (!last_tmp.empty()) fs::remove(last_tmp, err);
                            event_bus_.publish(FileProcessErrorEvent{file, "Pipeline failed", item.is_container});
                        }
                    } else {
                        // parallel
                        struct Result {
                            fs::path tmp;
                            uintmax_t size{};
                            bool success{false};
                        };
                        std::vector<Result> results;

                        for (std::size_t i = 0; i < candidates.size(); ++i) {
                            if (st.stop_requested()) break;
#ifdef __APPLE__
                            fs::path target_dir = has_output_dir_
                                ? (output_is_directory_ ? output_dir_ : output_dir_.parent_path())
                                : fs::temp_directory_path(); // use system temp to bypass sandbox restrictions
#else
                            // resolve target directory to keep temp file on the same mount point
                            fs::path target_dir = has_output_dir_
                                ? (output_is_directory_ ? output_dir_ : output_dir_.parent_path())
                                : file.parent_path();
#endif

                            fs::path tmp = target_dir / (file.filename().string() + "_" + job_suffix + ".pipe." + std::to_string(i) + ".tmp");
                            Result r{tmp, 0, false};
                            try {
                                candidates[i]->recompress(file, tmp, m_options);
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

                        auto best_it = std::min_element(results.begin(), results.end(),
                                                        [](const Result &a, const Result &b) {
                                                            if (a.success != b.success) return a.success && !b.success;
                                                            return a.size < b.size;
                                                        });

                        if (best_it != results.end() && best_it->success && best_it->size < orig_size) {
                            final_temp_path = best_it->tmp;
                            new_size = best_it->size;
                            success = true;
                            for (const auto &r: results) {
                                if (r.tmp != final_temp_path) {
                                    std::error_code ec2;
                                    fs::remove(r.tmp, ec2);
                                }
                            }
                        } else {
                            for (const auto &r: results) {
                                std::error_code ec;
                                fs::remove(r.tmp, ec);
                            }
                            if (!st.stop_requested()) {
                                Logger::log(LogLevel::Debug, "No size improvement, keeping original: " + file.string(), "Executor");
                                event_bus_.publish(FileProcessSkippedEvent{file, "No size improvement", item.is_container});
                            }
                        }
                    }

                    auto end = std::chrono::steady_clock::now();
                    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

                    if (success) {
                        auto move_result = move_to_destination(file, final_temp_path);
                        if (move_result) {
                            {
                                std::lock_guard<std::mutex> lock(recompressed_paths_mutex_);
                                recompressed_paths_[file.string()] = move_result->first;
                            }
                            event_bus_.publish(FileProcessCompleteEvent{
                                file,
                                move_result->first,
                                orig_size,
                                new_size,
                                move_result->second,
                                duration,
                                parent_container,
                                item.is_container
                            });
                        } else {
                            event_bus_.publish(FileProcessErrorEvent{file, "Failed to move optimized file", item.is_container});
                        }
                    } else if (st.stop_requested()) {
                        event_bus_.publish(FileProcessSkippedEvent{file, "Interrupted", item.is_container});
                    }
                } catch (const std::exception &e) {
                    Logger::log(LogLevel::Error, "Error on " + file.string() + ": " + std::string(e.what()), "Executor");
                    event_bus_.publish(FileProcessErrorEvent{file, e.what(), item.is_container});
                } catch (...) {
                    Logger::log(LogLevel::Error, "Unknown error on " + file.string(), "Executor");
                    event_bus_.publish(FileProcessErrorEvent{file, "Unknown non-standard exception", item.is_container});
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
                Logger::log(LogLevel::Warning, "No processor to finalize: " + content.original_path.string(), "Executor");
                event_bus_.publish(ContainerFinalizeErrorEvent{content.original_path, "Unsupported format"});
                continue;
            }

            try {
                // if Phase 2 already recompressed this same file, rebuild on top of
                // those bytes instead of the (possibly stale, with --output-dir) original
                ExtractedContent effective_content = content;
                {
                    std::lock_guard<std::mutex> lock(recompressed_paths_mutex_);
                    auto it = recompressed_paths_.find(content.original_path.string());
                    if (it != recompressed_paths_.end()) {
                        effective_content.original_path = it->second;
                    }
                }

                auto start = std::chrono::steady_clock::now();
                std::filesystem::path new_temp_file = procs.front()->finalize_extraction(effective_content, m_options);
                auto end = std::chrono::steady_clock::now();
                auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

                std::error_code ec;
                auto orig_size = content.original_size;

                if (new_temp_file.empty()) {
                    Logger::log(LogLevel::Debug, "Container finalize skipped (empty): " + content.original_path.string(), "Executor");
                    // publish explicit Phase 3 complete event even if skipped
                    event_bus_.publish(ContainerFinalizeCompleteEvent{content.original_path, content.original_path, orig_size, orig_size, false, duration});
                    continue;
                }

                auto new_size = std::filesystem::file_size(new_temp_file, ec);

                // Only enforce "must be strictly smaller" for processors that are
                // *pure* containers (can_recompress() == false), e.g. ArchiveProcessor,
                // OdfProcessor, OOXMLProcessor: for these, Phase 2 never touches the
                // original file, so falling back to it on a non-improving finalize is
                // always safe. Mixed processors (e.g. FlacProcessor, ApeProcessor,
                // MkvProcessor) already rebuild on top of Phase 2's recompressed bytes
                // (effective_content.original_path, redirected above via
                // recompressed_paths_) as part of reinserting an extracted resource
                // (like cover art) - discarding this result would mean serving Phase 2's
                // intermediate output with the pre-reinsertion resource still in place,
                // not losing anything, but the extraction/reinsertion round-trip itself
                // is treated as always worth keeping rather than re-compared by size.
                if (!procs.front()->can_recompress() && !ec && new_size >= orig_size) {
                    Logger::log(LogLevel::Debug,
                                "Container finalize discarded (no size improvement): " + content.original_path.string(),
                                "Executor");
                    std::filesystem::remove(new_temp_file, ec);
                    event_bus_.publish(ContainerFinalizeCompleteEvent{content.original_path, content.original_path, orig_size, orig_size, false, duration});
                    continue;
                }

                // use the helper and publish the specific Phase 3 event
                auto move_result = move_to_destination(content.original_path, new_temp_file);
                if (move_result) {
                    event_bus_.publish(ContainerFinalizeCompleteEvent{
                        content.original_path,
                        move_result->first,
                        orig_size,
                        ec ? 0 : new_size,
                        move_result->second,
                        duration
                    });
                } else {
                    event_bus_.publish(ContainerFinalizeErrorEvent{content.original_path, "Failed to finalize container file"});
                }

            } catch (const std::exception &e) {
                Logger::log(LogLevel::Error, "Finalize error: " + content.original_path.string() + " - " + std::string(e.what()), "Executor");
                event_bus_.publish(ContainerFinalizeErrorEvent{content.original_path, e.what()});
            }
        }
    }

    void ProcessorExecutor::request_stop() {
        stop_flag_.store(true, std::memory_order_relaxed);
        pool_.request_stop();
    }

} // namespace chisel