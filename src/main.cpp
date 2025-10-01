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
#include <random>
#include "utils/encoder_registry.hpp"
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

static void print_progress_bar(const size_t done, const size_t total, const double elapsed_seconds) {
    const unsigned term_width = get_terminal_width();
    const int bar_width = std::max(10u, term_width > 40u ? term_width - 40u : 20u);

    double progress = total ? static_cast<double>(done) / total : 0.0;
    int pos = static_cast<int>(bar_width * progress);

    std::cout << "\r[";
    for (int i = 0; i < bar_width; ++i) {
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

    if (files.empty()) {
        Logger::log(LogLevel::ERROR, "No files, folders or archives to process.", "main");
        return 1;
    }

    auto now = std::chrono::steady_clock::now();
    const auto start_total = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - start_total).count();
    if (!settings.is_pipe) {
        print_progress_bar(0, files.size(), elapsed);
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
    ThreadPool pool(settings.num_threads);
    std::mutex results_mutex;

    std::vector<std::future<void> > futures;
    size_t total_files = files.size();

    futures.reserve(files.size());
    for (auto const &in_path: files) {
        futures.push_back(pool.enqueue([&, in_path] {
            const auto start = std::chrono::steady_clock::now();
            const auto mime = detect_mime_type(in_path.string());
            const auto it = factories.find(mime);

            const uintmax_t sz_before = fs::file_size(in_path);
            uintmax_t sz_after_best = sz_before;
            bool ok_any = false, replaced = false;

            // new fields
            std::vector<std::pair<std::string, double> > codecs_used;
            std::string error_msg;

            if (it != factories.end()) {
                std::string best_tmp;
                // execute all encoders on copy of original
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
                            codecs_used.emplace_back(enc->name(), pct); // record codec + % reduction

                            Logger::log(LogLevel::DEBUG,
                                        "Encoder " + std::to_string(idx) + " → " + std::to_string(sz_after) + " bytes",
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
                            return 1;
                        }
                    } catch (...) {
                        fs::remove(tmp);
                        error_msg = "Unknown error";
                        if (settings.is_pipe) {
                            std::cerr << "Unknown error" << std::endl;
                            return 1;
                        }
                    }
                }

                // replace original if we have an improvement
                if (ok_any && sz_after_best < sz_before && !settings.dry_run) {
                    if (settings.is_pipe) {
                        // write new best to stdout
                        std::ifstream out_best(best_tmp, std::ios::binary);
                        std::cout << out_best.rdbuf();
                        out_best.close();

                        fs::remove(best_tmp);
                        fs::remove(in_path);
                        replaced = true;
                    } else {
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
                    }
                } else // no improvement or dry run
                    if (settings.is_pipe) {
                        // passthrough original
                        std::ifstream out_orig(in_path, std::ios::binary);
                        std::cout << out_orig.rdbuf();
                        out_orig.close();
                        if (!best_tmp.empty()) fs::remove(best_tmp);
                        fs::remove(in_path);
                    } else {
                        if (!best_tmp.empty()) fs::remove(best_tmp);
                    }
            } else {
                Logger::log(LogLevel::WARNING, "No encoder for " + in_path.string() + " (" + mime + ")", "main");
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
    for (auto &f: futures) f.get();

    // recreate extracted archives; recursion is handled inside these top level archives
    for (const auto &job: container_jobs) {
        ArchiveHandler archive_handler;
        archive_handler.finalize(job, settings);
    }
    if (!settings.is_pipe) {
        std::cout << "\n";
    }
    auto end_total = std::chrono::steady_clock::now();
    const double total_seconds = std::chrono::duration<double>(end_total - start_total).count();

        if (!settings.output_csv.empty()) {
            export_csv_report(results, settings.output_csv);
        } else {
            if (!settings.is_pipe) {
                print_console_report(results, settings.num_threads, total_seconds);
            }
        }

    return 0;
}
