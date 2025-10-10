//
// Created by Giuseppe Francione on 18/09/25.
//

#include <algorithm>
#include <iostream>
#include <filesystem>
#include <unordered_map>
#include <functional>
#include <vector>
#include <format>
#include "utils/file_type.hpp"
#include "encoder/encoder.hpp"
#include <sys/ioctl.h>
#include <string>
#include <atomic>
#include <mutex>
#include <chrono>
#include "cli/cli_parser.hpp"
#include "report/report_generator.hpp"
#include "utils/logger.hpp"
#include "utils/thread_pool.hpp"
#include "containers/archive_handler.hpp"
#include "utils/file_scanner.hpp"
#include <clocale>
#include <fstream>
#include "utils/encoder_registry.hpp"
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif
#include <csignal>
#include <atomic>

static std::atomic<bool> interrupted{false};

void signal_handler(int sig) {
    if (sig == SIGINT) {
        if (!interrupted.load()) {
            Logger::log(LogLevel::WARNING, "Stop detected. Killing current threads and saving results...", "main");
        } else {
            Logger::log(LogLevel::WARNING, "Still waiting for the threads to finish!", "main");
        }
        interrupted.store(true);

    }
}
static void print_progress_bar(const size_t done, const size_t total, const double elapsed_seconds) {
    const unsigned term_width = get_terminal_width();
    const unsigned int bar_width = std::max(10u, term_width > 40u ? term_width - 40u : 20u);

    const double progress = total ? static_cast<double>(done) / static_cast<double>(total) : 0.0;
    const unsigned pos = static_cast<int>(bar_width * progress);

    std::cout << "\r[";
    for (unsigned i = 0; i < bar_width; ++i) {
        if (i < pos) std::cout << "=";
        else if (i == pos) std::cout << ">";
        else std::cout << " ";
    }
    std::cout << "] "
            << std::format("{:3.0f}%", progress * 100.0)
            << " (" << done << "/" << total << ")"
            << " elapsed: " << std::format("{:.1f}s", elapsed_seconds)
            << std::flush;
}

inline void init_utf8_locale() {
    std::setlocale(LC_CTYPE, "");

    const char *cur = std::setlocale(LC_CTYPE, nullptr);
    if (cur && std::string(cur).find("UTF-8") != std::string::npos) {
        Logger::log(LogLevel::DEBUG, std::string("Current locale: ") + cur, "LocaleInit");
        return; // ok
    }

    constexpr const char *const fallbacks[] = {"C.UTF-8", "en_US.UTF-8", ".UTF-8" /* Windows */};
    for (const auto fb: fallbacks) {
        if (std::setlocale(LC_CTYPE, fb)) {
            Logger::log(LogLevel::INFO, std::string("Locale set to ") + fb, "LocaleInit");
            return;
        }
    }

    Logger::log(LogLevel::WARNING, "UTF-8 locale not available; non-ASCII file names may be problematic.",
                "LocaleInit");
}

namespace fs = std::filesystem;


int main(const int argc, char *argv[]) {
    std::signal(SIGINT, signal_handler);
    init_utf8_locale();
#ifdef _WIN32
    if (_isatty(_fileno(stdin)) == 0) {
        _setmode(_fileno(stdin), _O_BINARY);
    }
    if (_isatty(_fileno(stdout)) == 0) {
        _setmode(_fileno(stdout), _O_BINARY);
    }
#endif
    Settings settings;
    try {
        if (!parse_arguments(argc, argv, settings)) {
            return 1;
        }
    } catch (const std::exception &e) {
        Logger::log(LogLevel::ERROR, e.what(), "main");
        return 1;
    }


    // if requested, delete any existing magic.mgc so it will be reinstalled
    if (settings.regenerate_magic) {
        const auto path = get_magic_file_path();
        if (std::filesystem::exists(path)) {
            Logger::log(LogLevel::INFO, "Forcing regeneration of magic.mgc at " + path.string(), "libmagic");
            std::filesystem::remove(path);
        }
    }

    if (!std::filesystem::exists(get_magic_file_path())) {
        ensure_magic_installed();
    }

    auto factories = build_encoder_registry(settings.preserve_metadata);

    std::vector<fs::path> files;
    std::vector<ContainerJob> container_jobs;

    collect_inputs(settings.inputs, settings.recursive, files, container_jobs, settings);

    if (settings.is_pipe) {
        if (settings.dry_run) {
            std::cerr<<"Can't use dry run mode with pipe inputs!";
            return 1;
        }
        Logger::set_level(LogLevel::ERROR);
    }

    if (files.empty() && container_jobs.empty()) {
        Logger::log(LogLevel::ERROR, "No files, folders or archives to process.", "main");
        return 1;
    }

    auto now = std::chrono::steady_clock::now();
    const auto start_total = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - start_total).count();
    if (!settings.is_pipe) {
        print_progress_bar(0, files.size(), std::abs(elapsed));
    }
    std::ranges::sort(files,
                      [](const auto &a, const auto &b) {
                          return a.native() < b.native();
                      });

    if (settings.num_threads > files.size()) {
        settings.num_threads = files.size();
    }

    // we process each file
    std::vector<Result> results;
    std::vector<ContainerResult> container_results;
    ThreadPool pool(settings.num_threads);
    std::mutex results_mutex;

    std::vector<std::future<void> > futures;
    size_t total_files = files.size();

    futures.reserve(files.size());
    for (auto const &in_path: files) {
        futures.push_back(pool.enqueue([&, in_path] {
            const auto start = std::chrono::steady_clock::now();
            auto mime = detect_mime_type(in_path.string());
            if (mime.empty() || mime == "application/octet-stream") {
                std::string ext = in_path.extension().string();
                std::ranges::transform(ext, ext.begin(), ::tolower);
                auto ext_it = ext_to_mime.find(ext);
                if (ext_it != ext_to_mime.end()) {
                    Logger::log(LogLevel::DEBUG,
                                "MIME fallback: " + in_path.string() +
                                " detected as " + ext_it->second + " from extension " + ext,
                                "file_scanner");
                    mime = ext_it->second;
                }
            }
            const auto it = factories.find(mime);

            const uintmax_t sz_before = fs::file_size(in_path);
            uintmax_t sz_after_best = sz_before;
            bool ok_any = false, replaced = false;
            std::vector<std::pair<std::string, double> > codecs_used;
            std::string error_msg;

            if (it != factories.end()) {
                if (settings.encode_mode == EncodeMode::PIPE) {
                    // pipeline mode: apply encoders in sequence
                    fs::path current = in_path;
                    fs::path tmp;
                    bool pipeline_ok = true;

                    for (size_t idx = 0; idx < it->second.size(); ++idx) {
                        tmp = make_temp_path(in_path.stem(), in_path.extension().string());
                        auto enc = it->second[idx]();
                        try {
                            bool ok = enc->recompress(current, tmp);
                            if (!ok || !fs::exists(tmp)) {
                                fs::remove(tmp);
                                pipeline_ok = false;
                                break;
                            }
                            const uintmax_t sz_after = fs::file_size(tmp);
                            double pct = sz_before > 0
                                             ? 100.0 * (1.0 - static_cast<double>(sz_after) / sz_before)
                                             : 0.0;
                            codecs_used.emplace_back(enc->name(), pct);

                            if (current != in_path && fs::exists(current))
                                fs::remove(current);
                            current = tmp;
                        } catch (const std::exception &e) {
                            fs::remove(tmp);
                            error_msg = e.what();
                            pipeline_ok = false;
                            break;
                        } catch (...) {
                            fs::remove(tmp);
                            error_msg = "Unknown error";
                            pipeline_ok = false;
                            break;
                        }
                    }

                    if (pipeline_ok && fs::exists(current)) {
                        const uintmax_t sz_after = fs::file_size(current);
                        if (sz_after < sz_before && !settings.dry_run) {
                            try {
                                fs::rename(current, in_path);
                                replaced = true;
                                ok_any = true;
                                sz_after_best = sz_after;
                            } catch (const std::exception &e) {
                                Logger::log(LogLevel::ERROR,
                                            "Failed to replace original with optimized file: " + std::string(e.what()),
                                            "main");
                                fs::remove(current);
                                error_msg = e.what();
                            }
                        } else {
                            if (current != in_path && fs::exists(current))
                                fs::remove(current);
                        }
                    }
                } else {
                    // parallel mode: old behaviour
                    std::string best_tmp;
                    for (size_t idx = 0; idx < it->second.size(); ++idx) {
                        const auto enc = it->second[idx]();
                        fs::path tmp = make_temp_path(in_path.stem(), in_path.extension().string());
                        try {
                            const bool ok = enc->recompress(in_path, tmp.string());
                            if (ok && fs::exists(tmp)) {
                                const uintmax_t sz_after = fs::file_size(tmp);
                                double pct = sz_before > 0
                                                 ? 100.0 * (1.0 - static_cast<double>(sz_after) / sz_before)
                                                 : 0.0;
                                codecs_used.emplace_back(enc->name(), pct);

                                Logger::log(LogLevel::DEBUG,
                                            "Encoder " + std::to_string(idx) + " → " + std::to_string(sz_after) +
                                            " bytes",
                                            "main");
                                if (sz_after < sz_after_best) {
                                    if (!best_tmp.empty()) fs::remove(best_tmp);
                                    sz_after_best = sz_after;
                                    best_tmp = tmp.string();
                                    ok_any = true;
                                } else {
                                    fs::remove(tmp);
                                }
                            } else {
                                fs::remove(tmp);
                            }
                        } catch (const std::exception &e) {
                            fs::remove(tmp);
                            error_msg = e.what();
                            if (settings.is_pipe) {
                                std::cerr << e.what() << std::endl;
                            }
                        } catch (...) {
                            fs::remove(tmp);
                            error_msg = "Unknown error";
                        }
                    }

                    if (ok_any && sz_after_best < sz_before && !settings.dry_run) {
                        try {
                            fs::rename(best_tmp, in_path);
                            replaced = true;
                        } catch (const std::exception &e) {
                            Logger::log(LogLevel::ERROR,
                                        "Failed to replace original with optimized file: " + std::string(e.what()),
                                        "main");
                            fs::remove(best_tmp);
                            error_msg = e.what();
                        }
                    } else {
                        if (!best_tmp.empty()) fs::remove(best_tmp);
                    }
                }
            } else {
                Logger::log(LogLevel::WARNING,
                            "No encoder for " + in_path.string() + " (" + mime + ")", "main");
                error_msg = "No encoder available";
            }

            const auto end = std::chrono::steady_clock::now();
            const double seconds = std::chrono::duration<double>(end - start).count();

            const std::scoped_lock lock(results_mutex);
            Result r;
            r.filename = in_path.string();
            r.mime = mime;
            r.size_before = sz_before;
            r.size_after = sz_after_best;
            r.success = ok_any;
            r.replaced = replaced;
            r.seconds = seconds;
            r.codecs_used = std::move(codecs_used);
            r.error_msg = error_msg;
            results.push_back(std::move(r)); {
                static std::mutex print_mutex;
                static std::atomic<size_t> count{0};
                const size_t done = ++count;
                const std::scoped_lock lock1(print_mutex);
                auto now = std::chrono::steady_clock::now();
                double elapsed = std::chrono::duration<double>(now - start_total).count();
                if (!settings.is_pipe) {
                    print_progress_bar(done, total_files, elapsed);
                }
            }
        }));
    }
    // wait for all tasks to finish
    for (auto &f: futures) {
        f.get();
        if (interrupted.load()) {
            break;
        }
    }

    if (settings.is_pipe && !results[0].error_msg.empty()) {
        std::cerr<<"Encoding returner error " + results[0].error_msg;
        return 1;
    }
    // recreate extracted archives; recursion is handled inside these top level archives
    for (const auto &job: container_jobs) {
        auto handler = make_handler(job.format);
        auto before = fs::file_size(job.original_path);
        handler->finalize(job, settings);
        auto after = fs::file_size(job.original_path);
        ContainerResult cr{job.original_path, container_format_to_string(job.format), before, after, true, ""};
        container_results.push_back(std::move(cr));
    }
    if (!settings.is_pipe) {
        std::cout << "\n";
    }
    auto end_total = std::chrono::steady_clock::now();
    const double total_seconds = std::chrono::duration<double>(end_total - start_total).count();
    if (!results.empty()) {
        if (!settings.output_csv.empty()) {
            export_csv_report(results, container_results, settings.output_csv, total_seconds, settings.encode_mode);
        } else {
            if (!settings.is_pipe) {
                print_console_report(results, container_results, settings.num_threads, total_seconds, settings.encode_mode);
            }
        }
    }
    if (interrupted.load()) {
        return 130;
    }
    return 0;
}
