//
// Created by Giuseppe Francione on 20/09/25.
//

#include "file_scanner.hpp"
#include "file_type.hpp"
#include "archive_formats.hpp"
#include "logger.hpp"
#include <algorithm>
#include "../containers/mkv_handler.hpp"

namespace fs = std::filesystem;

void collect_inputs(const std::vector<fs::path>& inputs,
                    bool recursive,
                    std::vector<fs::path>& files,
                    std::vector<ContainerJob>& archive_jobs) {
    for (auto const& p : inputs) {
        if (!fs::exists(p)) {
            Logger::log(LogLevel::ERROR, "Scanner error: input '" + p.string() + "' not found.", "file_scanner");
            continue;
        }

        if (fs::is_directory(p)) {
            if (recursive) {
                for (auto& e : fs::recursive_directory_iterator(p)) {
                    if (fs::is_regular_file(e.path())) {
                        files.push_back(e.path());
                    }
                }
            } else {
                for (auto& e : fs::directory_iterator(p)) {
                    if (fs::is_regular_file(e.path())) {
                        files.push_back(e.path());
                    }
                }
            }
        } else {
            auto filename = p.filename().string();
            std::string lower = filename;
            std::ranges::transform(lower, lower.begin(), ::tolower);

            if (lower != ".ds_store" && lower != "desktop.ini") {
                auto mime = detect_mime_type(p.string());

                // fallback: if MIME empty, octet-stream or unknown, try with extension
                if (mime.empty() || mime == "application/octet-stream" ||
                    (mime_to_format.find(mime) == mime_to_format.end() && ext_to_mime.find(p.extension().string()) != ext_to_mime.end())) {

                    std::string ext = p.extension().string();
                    std::ranges::transform(ext, ext.begin(), ::tolower);
                    auto ext_it = ext_to_mime.find(ext);
                    if (ext_it != ext_to_mime.end()) {
                        Logger::log(LogLevel::DEBUG, "MIME fallback: " + p.string() +
                                                     " detected as " + ext_it->second + " from extension " + ext, "file_scanner");
                        mime = ext_it->second;
                    }
                }


                // if it's a supported container
                auto fmt_it = mime_to_format.find(mime);
                if (fmt_it != mime_to_format.end()) {
                    const auto fmt = fmt_it->second;

                    if (can_read_format(fmt)) {
                        if (fmt == ContainerFormat::Mkv) {
                            // mkv
                            MKVHandler mkv_handler;
                            auto job = mkv_handler.prepare(p.string());
                            if (!job.file_list.empty()) {
                                archive_jobs.push_back(job);
                                for (const auto& f : job.file_list) {
                                    files.push_back(f);
                                }
                            }
                        } else {
                            // archives
                            ArchiveHandler archive_handler;
                            auto job = archive_handler.prepare(p.string());
                            if (!job.file_list.empty()) {
                                archive_jobs.push_back(job);
                                for (const auto& f : job.file_list) {
                                    files.push_back(f);
                                }
                            }
                        }
                    }
                } else {
                    // normal file
                    files.push_back(p);
                }
            }
        }
    }

    if (files.empty()) {
        Logger::log(LogLevel::ERROR, "Scanner error: no file, folder or archive specified.", "file_scanner");
    }
}