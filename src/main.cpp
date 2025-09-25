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
#include "encoder/flac_encoder.hpp"
#include "encoder/png_encoder.hpp"
#include <sys/ioctl.h>
#include <string>
#include <atomic>
#include <mutex>
#include <chrono>
#include "cli/cli_parser.hpp"
#include "encoder/jpeg_encoder.hpp"
#include "report/report_generator.hpp"
#include "utils/logger.hpp"
#include "utils/thread_pool.hpp"
#include "containers/archive_handler.hpp"
#include "utils/file_scanner.hpp"
#include <clocale>

inline void init_utf8_locale() {
    std::setlocale(LC_ALL, "");

    const char *cur = std::setlocale(LC_CTYPE, nullptr);
    if (cur && std::string(cur).find("UTF-8") != std::string::npos) {
        Logger::log(LogLevel::DEBUG, std::string("Locale corrente: ") + cur, "LocaleInit");
        return; // ok
    }

    const char *fallbacks[] = {"C.UTF-8", "en_US.UTF-8", ".UTF-8" /* Windows */};
    for (const auto fb: fallbacks) {
        if (std::setlocale(LC_ALL, fb)) {
            Logger::log(LogLevel::INFO, std::string("Locale set to ") + fb, "LocaleInit");
            return;
        }
    }

    // no UTF-8 available
    Logger::log(LogLevel::WARNING, "UTF-8 locale not available; non-ASCII file names may be problematic.",
                "LocaleInit");
}

namespace fs = std::filesystem;


int main(const int argc, char *argv[]) {
    init_utf8_locale();
    Settings settings;
    try {
        if (!parse_arguments(argc, argv, settings)) {
            return 1;
        }
    } catch (const std::exception &e) {
        Logger::log(LogLevel::ERROR, e.what(), "main");
        return 1;
    }

    // register encoders
    using Factory = std::function<std::unique_ptr<IEncoder>()>;
    std::unordered_map<std::string, std::vector<Factory> > factories;

    factories["audio/flac"] = {
        [settings] {
            return std::make_unique<FlacEncoder>(settings.preserve_metadata);
        }
    };

    factories["audio/x-flac"] = factories["audio/flac"];

    factories["image/png"] = {
        [settings] {
            return std::make_unique<PngEncoder>(settings.preserve_metadata);
        }
    };
    factories["image/jpeg"] = {
        [settings] {
            return std::make_unique<JpegEncoder>(settings.preserve_metadata);
        }
    };
    factories["image/jpg"] = factories["image/jpeg"];

    std::vector<fs::path> files;
    std::vector<ContainerJob> container_jobs;

    collect_inputs(settings.inputs, settings.recursive, files, container_jobs);

    if (files.empty()) {
        Logger::log(LogLevel::ERROR, "No files, folders or archives to process.", "main");
        return 1;
    }

    std::cout << "\r[" << 0 << "/" << files.size() << "] completati ("
            << std::format("{:.1f}", 100.0 * 0 / files.size()) << "%)   "
            << std::flush;

    std::ranges::sort(files,
                      [](auto const &a, auto const &b) {
                          return a.string() < b.string();
                      });
    if (settings.num_threads > files.size()) {
        settings.num_threads = files.size();
    }

    // elaboriamo ogni file
    std::vector<Result> results;
    ThreadPool pool(settings.num_threads);
    std::mutex results_mutex;

    std::vector<std::future<void> > futures;
    size_t total_files = files.size();
    const auto start_total = std::chrono::steady_clock::now();

    for (auto const &in_path: files) {
        futures.push_back(pool.enqueue([&, in_path] {
            const auto start = std::chrono::steady_clock::now();
            const auto mime = detect_mime_type(in_path.string());
            const auto it = factories.find(mime);

            const uintmax_t sz_before = fs::file_size(in_path);
            uintmax_t sz_after_best = sz_before;
            bool ok_any = false, replaced = false;

            if (it != factories.end()) {
                std::string best_tmp;
                // execute all encoders on copy of original
                for (size_t idx = 0; idx < it->second.size(); ++idx) {
                    const auto enc = it->second[idx]();
                    fs::path tmp = in_path.stem().string() + "_tmp" + std::to_string(idx) + in_path.extension().
                                   string();
                    try {
                        bool ok = enc->recompress(in_path, tmp.string());
                        if (ok && fs::exists(tmp)) {
                            const uintmax_t sz_after = fs::file_size(tmp);
                            Logger::log(LogLevel::DEBUG,
                                        "Encoder " + std::to_string(idx) + " → " + std::to_string(sz_after) + " bytes",
                                        "main");
                            if (sz_after < sz_after_best) {
                                // remove previous best
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
                    } catch (...) {
                        fs::remove(tmp);
                    }
                }

                // replace original if we have an improvement
                if (ok_any && sz_after_best < sz_before && !settings.dry_run) {
                    fs::rename(best_tmp, in_path);
                    replaced = true;
                } else if (!best_tmp.empty()) {
                    fs::remove(best_tmp);
                }
            } else {
                Logger::log(LogLevel::WARNING, "No encoder for " + in_path.string() + " (" + mime + ")", "main");
            }

            const auto end = std::chrono::steady_clock::now();
            const double seconds = std::chrono::duration<double>(end - start).count();

            std::scoped_lock lock(results_mutex);
            results.push_back({in_path.string(), mime, sz_before, sz_after_best, ok_any, replaced, seconds}); {
                static std::mutex cout_mutex;
                static std::atomic<size_t> count{0};
                const size_t done = ++count;
                std::scoped_lock lock1(cout_mutex);
                std::cout << "\r[" << done << "/" << total_files << "] completed ("
                        << std::format("{:.1f}", (100.0 * done / total_files)) << "%)   "
                        << std::flush;
            }
        }));
    }

    // wait for all tasks to finish
    for (auto &f: futures) f.get();

    // recreate extracted archives
    for (const auto &job: container_jobs) {
        ArchiveHandler archive_handler;
        archive_handler.finalize(job, settings);
    }

    std::cout << "\n";
    auto end_total = std::chrono::steady_clock::now();
    double total_seconds = std::chrono::duration<double>(end_total - start_total).count();

    if (!settings.output_csv.empty()) {
        export_csv_report(results, settings.output_csv);
    } else {
        print_console_report(results, settings.num_threads, total_seconds);
    }


    return 0;
}
