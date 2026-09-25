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
#include <algorithm>
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
    namespace {
        bool is_junk(const fs::path& path) {
            auto name = path.filename().string();
            std::transform(name.begin(), name.end(), name.begin(), ::tolower);
            return name == ".ds_store" || name == "desktop.ini" || name.starts_with("._");
        }

        fs::path normalized(const fs::path& path) {
            std::error_code ec;
            auto result = fs::absolute(path, ec);
            if (ec) result = path;
            result = result.lexically_normal();
            // drop a trailing separator
            if (!result.has_filename() && result.has_relative_path()) result = result.parent_path();
            return result;
        }

        // deepest folder holding every root, a folder root counting as itself
        fs::path common_parent(const std::vector<fs::path>& roots) {
            std::optional<fs::path> common;
            for (const auto& root : roots) {
                auto dir = normalized(root);
                if (!fs::is_directory(dir)) dir = dir.parent_path();
                if (!common) {
                    common = dir;
                    continue;
                }
                fs::path prefix;
                for (auto a = common->begin(), b = dir.begin(); a != common->end() && b != dir.end() && *a == *b; ++a, ++b) {
                    prefix /= *a;
                }
                common = prefix;
            }
            return common.value_or(fs::path{});
        }

        // writes source's bytes over target from the start and truncates it only at the end
        bool overwrite_contents(const fs::path& source, const fs::path& target) {
            std::ifstream in(source, std::ios::binary);
            std::fstream out(target, std::ios::binary | std::ios::in | std::ios::out);
            if (!in || !out) return false;
            std::vector<char> buffer(1 << 20);
            std::uintmax_t size = 0;
            while (in) {
                in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                const auto got = in.gcount();
                if (got <= 0) break;
                out.write(buffer.data(), got);
                if (!out) return false;
                size += static_cast<std::uintmax_t>(got);
            }
            if (in.bad()) return false;
            out.close();
            if (out.fail()) return false;
            std::error_code ec;
            fs::resize_file(target, size, ec);
            return !ec;
        }

        // overwrites target in place, putting it back from a backup if that fails halfway
        std::error_code overwrite_keeping_backup(const fs::path& temp, const fs::path& target) {
            std::error_code ec;
            const auto backup = fs::temp_directory_path() /
                                (target.filename().string() + ".chisel-backup-" + RandomUtils::random_suffix());
            if (!fs::copy_file(target, backup, fs::copy_options::overwrite_existing, ec) || ec) {
                std::error_code rm_ec;
                fs::remove(backup, rm_ec);
                return ec ? ec : std::make_error_code(std::errc::io_error);
            }
            if (overwrite_contents(temp, target)) {
                fs::remove(backup, ec);
                fs::remove(temp, ec);
                return {};
            }
            if (overwrite_contents(backup, target)) {
                fs::remove(backup, ec);
            } else {
                Logger::log(LogLevel::Error, "Can't restore " + target.string() + ", its original content is in " +
                            backup.string(), "Executor");
            }
            return std::make_error_code(std::errc::io_error);
        }

        // copies temp next to target and renames it over target, so target is never left half-written;
        // where nothing can be created next to it (sandboxed apps on macOS), overwrites it in place instead
        std::error_code replace_via_copy(const fs::path& temp, const fs::path& target) {
            std::error_code ec;
            const auto sibling = target.parent_path() / (".chisel-" + RandomUtils::random_suffix() + ".tmp");
            if (fs::copy_file(temp, sibling, fs::copy_options::overwrite_existing, ec) && !ec) {
                fs::rename(sibling, target, ec);
                if (!ec) {
                    fs::remove(temp, ec);
                    return {};
                }
            }
            std::error_code rm_ec;
            fs::remove(sibling, rm_ec);
#ifdef __APPLE__
            if (fs::exists(target, rm_ec)) return overwrite_keeping_backup(temp, target);
#endif
            return ec ? ec : std::make_error_code(std::errc::io_error);
        }
    } // namespace

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

    void ProcessorExecutor::process(const std::vector<fs::path> &inputs, const std::vector<fs::path> &roots) {
        struct DryRunDirGuard {
            const fs::path& dir;
            ~DryRunDirGuard() {
                std::error_code ec;
                if (!dir.empty()) fs::remove_all(dir, ec);
            }
        } dry_run_dir_guard{.dir=dry_run_dir_};

        if (dry_run_) {
            dry_run_dir_ = fs::temp_directory_path() / ("chisel-dry-run-" + RandomUtils::random_suffix());
        } else if (has_output_dir_) {
            const auto& layout_roots = roots.empty() ? inputs : roots;
            output_is_directory_ = layout_roots.size() != 1 || !fs::is_regular_file(layout_roots.front()) ||
                                   fs::is_directory(output_dir_) || !output_dir_.has_filename();
            std::error_code ec;
            if (output_is_directory_) {
                output_base_ = common_parent(layout_roots);
                fs::create_directories(output_dir_, ec);
                if (ec) {
                    Logger::log(LogLevel::Error, "Failed to create output directory: " + output_dir_.string(), "Executor");
                    return;
                }
                for (const auto &path: inputs) {
                    fs::create_directories(destination_for(path).parent_path(), ec);
                }
            } else if (output_dir_.has_parent_path()) {
                fs::create_directories(output_dir_.parent_path(), ec);
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
        if (stop_flag_.load(std::memory_order_relaxed)) return;
        copy_unchanged_inputs(inputs);
    }

    std::optional<std::pair<fs::path, bool>> ProcessorExecutor::move_to_destination(
        const fs::path& original_file,
        const fs::path& temp_file,
        const bool nested) {

        std::error_code ec;
        const auto new_size = fs::file_size(temp_file, ec);
        if (ec || new_size == 0) {
            Logger::log(LogLevel::Warning, "Temp file is invalid or empty: " + temp_file.string(), "Executor");
            fs::remove(temp_file, ec);
            return std::nullopt;
        }

        bool replaced = false;
        fs::path dest = original_file;

        if (dry_run_ && !nested) {
            Logger::log(LogLevel::Info, "[DRY-RUN] Would replace: " + original_file.string(), "Executor");
            fs::remove(temp_file, ec);
        } else if (has_output_dir_ && !nested) {
            dest = destination_for(original_file);

            int retries = 10;
            while (retries > 0) {
                fs::rename(temp_file, dest, ec);
                if (ec == std::errc::cross_device_link) ec = replace_via_copy(temp_file, dest);
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
            std::lock_guard<std::mutex> lock(recompressed_paths_mutex_);
            outputs_written_.insert(dest.string());
        } else {
            // in-place
            int retries = 10;
            while (retries > 0) {
                fs::rename(temp_file, original_file, ec);
#ifdef __APPLE__
                // the temp file sits on the system volume, and sandboxed apps may not rename into the folder
                if (ec) ec = replace_via_copy(temp_file, original_file);
#else
                if (ec == std::errc::cross_device_link) ec = replace_via_copy(temp_file, original_file);
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
            if (ec) {
                Logger::log(LogLevel::Error, "Rename failed: " + original_file.string() + " (" + ec.message() + ")", "Executor");
                fs::remove(temp_file, ec);
                return std::nullopt;
            }
        }

        // a dry run still updates extracted files, so containers report their real size
        return std::make_pair(dest, replaced && !dry_run_);
    }

    fs::path ProcessorExecutor::temp_dir_for([[maybe_unused]] const fs::path& file, const bool nested) const {
        if (has_output_dir_ && !nested && !dry_run_) {
            return destination_for(file).parent_path();
        }
#ifdef __APPLE__
        return fs::temp_directory_path(); // use system temp to bypass sandbox restrictions
#else
        // keep the temp file on the same mount point; a dry run never writes next to the inputs
        return dry_run_ ? fs::temp_directory_path() : file.parent_path();
#endif
    }

    fs::path ProcessorExecutor::destination_for(const fs::path& input) const {
        if (!output_is_directory_) return output_dir_;
        const auto path = normalized(input);
        auto relative = path.lexically_relative(output_base_);
        if (relative.empty() || *relative.begin() == "..") {
            // no common parent (inputs on different drives): keep the whole path under the drive's name
            auto root = path.root_name().string();
            std::erase_if(root, [](const char c) { return c == ':' || c == '/' || c == '\\'; });
            relative = fs::path(root) / path.relative_path();
        }
        return output_dir_ / relative;
    }

    std::optional<fs::path> ProcessorExecutor::keep_for_finalize(const fs::path& file, const fs::path& temp_file) const {
        // own folder per file: finalizers may pick the format from the extension
        const auto kept = dry_run_dir_ / RandomUtils::random_suffix() / file.filename();
        std::error_code ec;
        fs::create_directories(kept.parent_path(), ec);
        if (!ec) fs::rename(temp_file, kept, ec);
        if (ec) {
            Logger::log(LogLevel::Warning, "Failed to keep the dry run result of " + file.string() + " (" + ec.message() + ")", "Executor");
            fs::remove(temp_file, ec);
            return std::nullopt;
        }
        return kept;
    }

    void ProcessorExecutor::copy_unchanged_inputs(const std::vector<fs::path>& inputs) const {
        if (!has_output_dir_ || dry_run_) return;
        for (const auto &input: inputs) {
            if (stop_flag_.load(std::memory_order_relaxed)) return;
            const auto dest = destination_for(input);
            if (is_junk(input) || outputs_written_.contains(dest.string())) continue;
            std::error_code ec;
            if (fs::equivalent(input, dest, ec)) continue;
            fs::copy_file(input, dest, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                Logger::log(LogLevel::Error, "Failed to copy " + input.string() + " to " + dest.string() + " (" + ec.message() + ")", "Executor");
            } else {
                Logger::log(LogLevel::Debug, "Copied unchanged: " + input.string(), "Executor");
            }
        }
    }

    void ProcessorExecutor::analyze_path(const fs::path &path, const std::optional<fs::path>& parent, const unsigned depth,
                                         const bool keep_pixel_format) {
        if (stop_flag_.load(std::memory_order_relaxed)) return;

        if (depth > kMaxNestingDepth) {
            Logger::log(LogLevel::Error,
                        "Maximum container nesting depth (" + std::to_string(kMaxNestingDepth) +
                        ") exceeded, refusing to descend further into: " + path.string(),
                        "Executor");
            event_bus_.publish(FileAnalyzeSkippedEvent{.path=path, .reason="Maximum nesting depth exceeded"});
            return;
        }

        if (is_junk(path)) {
            event_bus_.publish(FileAnalyzeSkippedEvent{.path=path, .reason="Junk file"});

            return;
        }

        // nothing can shrink an empty file, and processors take an empty output for a failure
        if (std::error_code size_ec; fs::file_size(path, size_ec) == 0 && !size_ec) {
            event_bus_.publish(FileAnalyzeSkippedEvent{.path=path, .reason="Empty file"});
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
            event_bus_.publish(FileAnalyzeSkippedEvent{.path=path, .reason="Unsupported format"});
            return;
        }

        IProcessor *processor = procs.front();

        if (!m_options.break_signatures &&
            std::ranges::any_of(procs, [&path](const IProcessor* p) { return p->is_signed(path); })) {
            Logger::log(LogLevel::Warning, "Digitally signed, left untouched: " + path.string(), "Executor");
            event_bus_.publish(FileAnalyzeSkippedEvent{.path=path, .reason="Digitally signed", .is_signed=true});
            return;
        }

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
                finalize_stack_.push({.content=*content, .nested=parent.has_value()});
                for (const auto &child: content->extracted_files) {
                    analyze_path(child, path, depth + 1, content->fixed_pixel_format.contains(child));
                }
                scheduled_for_extraction = true;
            } else {
                if (processor->can_recompress()) {
                    Logger::log(LogLevel::Warning, "Prepare_extraction resulted in no elements for " + path.string(), "Executor");
                    event_bus_.publish(FileAnalyzeSkippedEvent{.path=path, .reason="Extraction resulted in no elements"});
                } else {
                    Logger::log(LogLevel::Warning, "Prepare_extraction skipped or resulted in no elements for " + path.string(), "Executor");
                    event_bus_.publish(FileAnalyzeErrorEvent{.path=path, .error_message="Extraction failed or skipped"});
                }
            }
        }
        if (processor->can_recompress()) {
            work_list_.push_back({.path=current_path, .parent_container=parent, .is_container=scheduled_for_extraction,
                                  .keep_pixel_format=keep_pixel_format});
            scheduled_for_recompression = true;
        }
        if (scheduled_for_extraction || scheduled_for_recompression) {
            if (scheduled_for_extraction) {
                event_bus_.publish(FileAnalyzeCompleteEvent{.path=path, .extracted=true, .scheduled=scheduled_for_recompression, .num_children=content->extracted_files.size(), .depth=depth});
            } else {
                event_bus_.publish(FileAnalyzeCompleteEvent{.path=path, .extracted=false, .scheduled=scheduled_for_recompression, .num_children=0, .depth=depth});
            }
        } else {
            Logger::log(LogLevel::Debug, "File ignored: " + path.string(), "Executor");
            event_bus_.publish(FileAnalyzeSkippedEvent{.path=path, .reason="No operations available"});
        }
    }

    void ProcessorExecutor::process_work_list() {
        for (const auto &item: work_list_) {
            if (stop_flag_.load(std::memory_order_relaxed)) return;
            pool_.enqueue([this, item](stop_token st) {
                const auto& file = item.path;
                const auto& parent_container = item.parent_container;
                const bool nested = parent_container.has_value();
                ProcessingOptions options = m_options;
                options.keep_pixel_format = item.keep_pixel_format;
                if (st.stop_requested()) {
                    event_bus_.publish(FileProcessSkippedEvent{.path=file, .reason="Interrupted", .is_container=item.is_container});
                    return;
                }
                event_bus_.publish(FileProcessStartEvent{.path=file, .parent_container=parent_container, .is_container=item.is_container});

                // collect all candidates
                auto candidates = registry_.find_by_mime(MimeDetector::detect(file));
                if (candidates.empty()) {
                    candidates = registry_.find_by_extension(file.extension().string());
                }
                if (candidates.empty()) {
                    Logger::log(LogLevel::Warning, "No processor for " + file.string(), "Executor");
                    event_bus_.publish(FileAnalyzeSkippedEvent{.path=file, .reason="Unsupported format"});
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
                        } last_tmp_guard{.path=last_tmp, .release=false};

                        for (std::size_t i = 0; i < candidates.size(); ++i) {
                            if (st.stop_requested()) {
                                pipeline_ok = false;
                                break;
                            }

                            fs::path tmp = temp_dir_for(file, nested) / (file.filename().string() + "_" + job_suffix + ".pipe." + std::to_string(i) + ".tmp");
                            struct TempFileGuard {
                                fs::path path;
                                bool release = false;
                                ~TempFileGuard() {
                                    if (!release && !path.empty()) {
                                        std::error_code ec;
                                        fs::remove(path, ec);
                                    }
                                }
                            } tmp_guard{.path=tmp, .release=false};

                            bool stage_ok;
                            try {
                                candidates[i]->recompress(current, tmp, options);
                                stage_ok = safe_size(tmp) > 0;
                            } catch (const std::exception& e) {
                                Logger::log(LogLevel::Warning, "Pipeline stage " + std::to_string(i) + " (" +
                                            std::string(candidates[i]->get_name()) + ") failed for " + file.string() +
                                            ": " + e.what(), "Executor");
                                stage_ok = false;
                            } catch (...) {
                                Logger::log(LogLevel::Warning, "Pipeline stage " + std::to_string(i) + " (" +
                                            std::string(candidates[i]->get_name()) + ") failed for " + file.string() +
                                            " with a non-standard exception", "Executor");
                                stage_ok = false;
                            }

                            if (!stage_ok) {
                                // a later stage failing still leaves the previous stage's
                                // (already smaller) output usable; only bail out entirely
                                // if nothing has succeeded yet
                                if (last_tmp.empty()) {
                                    pipeline_ok = false;
                                } else {
                                    Logger::log(LogLevel::Debug, "Falling back to stage " + std::to_string(i - 1) +
                                                "'s output for " + file.string(), "Executor");
                                }
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
                            // only a result that gets kept needs checking
                            const bool checksum_ok = !size_improved || !m_options.verify_checksums ||
                                candidates[0]->raw_equal(file, last_tmp);

                            if (size_improved && checksum_ok) {
                                final_temp_path = last_tmp;
                                success = true;
                                last_tmp_guard.release = true; // ownership transferred to final_temp_path
                            } else {
                                std::error_code ec;
                                fs::remove(last_tmp, ec);
                                if (!checksum_ok) {
                                    event_bus_.publish(FileProcessErrorEvent{.path=file, .error_message="INTEGRITY CHECK FAILED: Data corruption detected", .is_container=item.is_container});
                                } else {
                                    Logger::log(LogLevel::Debug, "No size improvement, keeping original: " + file.string(), "Executor");
                                    event_bus_.publish(FileProcessSkippedEvent{.path=file, .reason="No size improvement", .is_container=item.is_container});
                                }
                            }
                        } else if (!st.stop_requested()) {
                            auto err = std::error_code{};
                            if (!last_tmp.empty()) fs::remove(last_tmp, err);
                            event_bus_.publish(FileProcessErrorEvent{.path=file, .error_message="Pipeline failed", .is_container=item.is_container});
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

                            fs::path tmp = temp_dir_for(file, nested) / (file.filename().string() + "_" + job_suffix + ".pipe." + std::to_string(i) + ".tmp");
                            Result r{.tmp=tmp, .size=0, .success=false};
                            try {
                                candidates[i]->recompress(file, tmp, options);
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
                                event_bus_.publish(FileProcessSkippedEvent{.path=file, .reason="No size improvement", .is_container=item.is_container});
                            }
                        }
                    }

                    auto end = std::chrono::steady_clock::now();
                    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

                    if (success) {
                        std::optional<std::pair<fs::path, bool>> move_result;
                        fs::path result_path;
                        if (dry_run_ && !nested && item.is_container) {
                            // a dry run keeps a container's result for Phase 3 to rebuild on
                            if (auto kept = keep_for_finalize(file, final_temp_path)) {
                                result_path = *kept;
                                move_result.emplace(file, false);
                            }
                        } else if ((move_result = move_to_destination(file, final_temp_path, nested))) {
                            result_path = move_result->first;
                        }
                        if (move_result) {
                            {
                                std::lock_guard<std::mutex> lock(recompressed_paths_mutex_);
                                recompressed_paths_[file.string()] = result_path;
                            }
                            event_bus_.publish(FileProcessCompleteEvent{
                                .path=file,
                                .destination=move_result->first,
                                .original_size=orig_size,
                                .new_size=new_size,
                                .replaced=move_result->second,
                                .duration=duration,
                                .parent_container=parent_container,
                                .is_container=item.is_container
                            });
                        } else {
                            event_bus_.publish(FileProcessErrorEvent{.path=file, .error_message="Failed to move optimized file", .is_container=item.is_container});
                        }
                    } else if (st.stop_requested()) {
                        event_bus_.publish(FileProcessSkippedEvent{.path=file, .reason="Interrupted", .is_container=item.is_container});
                    }
                } catch (const std::exception &e) {
                    Logger::log(LogLevel::Error, "Error on " + file.string() + ": " + std::string(e.what()), "Executor");
                    event_bus_.publish(FileProcessErrorEvent{.path=file, .error_message=e.what(), .is_container=item.is_container});
                } catch (...) {
                    Logger::log(LogLevel::Error, "Unknown error on " + file.string(), "Executor");
                    event_bus_.publish(FileProcessErrorEvent{.path=file, .error_message="Unknown non-standard exception", .is_container=item.is_container});
                }
            });
        }
        pool_.wait_idle();
    }

    void ProcessorExecutor::finalize_containers() {
        while (!finalize_stack_.empty() && !stop_flag_.load()) {
            auto [content, nested] = finalize_stack_.top();
            finalize_stack_.pop();

            event_bus_.publish(ContainerFinalizeStartEvent{content.original_path});

            auto procs = registry_.find_by_mime(MimeDetector::detect(content.original_path));
            if (procs.empty()) {
                procs = registry_.find_by_extension(content.original_path.extension().string());
            }
            if (procs.empty()) {
                Logger::log(LogLevel::Warning, "No processor to finalize: " + content.original_path.string(), "Executor");
                event_bus_.publish(ContainerFinalizeErrorEvent{.path=content.original_path, .error_message="Unsupported format"});
                continue;
            }

            try {
                // if Phase 2 already recompressed this same file, rebuild on top of
                // those bytes instead of the (possibly stale, with --output-dir) original
                ExtractedContent effective_content = content;
                bool recompressed = false;
                {
                    std::lock_guard<std::mutex> lock(recompressed_paths_mutex_);
                    auto it = recompressed_paths_.find(content.original_path.string());
                    if (it != recompressed_paths_.end()) {
                        effective_content.original_path = it->second;
                        recompressed = true;
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
                    // publish explicit Phase 3 complete event even if skipped, phase 2's result being the final one
                    auto final_size = orig_size;
                    if (recompressed) {
                        if (const auto size = std::filesystem::file_size(effective_content.original_path, ec); !ec) final_size = size;
                    }
                    const bool replaced = recompressed && !dry_run_;
                    event_bus_.publish(ContainerFinalizeCompleteEvent{.path=content.original_path, .destination=replaced ? effective_content.original_path : content.original_path, .original_size=orig_size, .final_size=final_size, .replaced=replaced, .duration=duration});
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
                    event_bus_.publish(ContainerFinalizeCompleteEvent{.path=content.original_path, .destination=content.original_path, .original_size=orig_size, .final_size=orig_size, .replaced=false, .duration=duration});
                    continue;
                }

                // use the helper and publish the specific Phase 3 event
                auto move_result = move_to_destination(content.original_path, new_temp_file, nested);
                if (move_result) {
                    event_bus_.publish(ContainerFinalizeCompleteEvent{
                        .path=content.original_path,
                        .destination=move_result->first,
                        .original_size=orig_size,
                        .final_size=ec ? 0 : new_size,
                        .replaced=move_result->second,
                        .duration=duration
                    });
                } else {
                    event_bus_.publish(ContainerFinalizeErrorEvent{.path=content.original_path, .error_message="Failed to finalize container file"});
                }

            } catch (const std::exception &e) {
                Logger::log(LogLevel::Error, "Finalize error: " + content.original_path.string() + " - " + std::string(e.what()), "Executor");
                event_bus_.publish(ContainerFinalizeErrorEvent{.path=content.original_path, .error_message=e.what()});
            }
        }
    }

    void ProcessorExecutor::request_stop() {
        stop_flag_.store(true, std::memory_order_relaxed);
        pool_.request_stop();
    }

} // namespace chisel