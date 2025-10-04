//
// Created by Giuseppe Francione on 20/09/25.
//

#include "file_scanner.hpp"
#include "file_type.hpp"
#include "archive_formats.hpp"
#include "logger.hpp"
#include <algorithm>
#include <fstream>

#include "../containers/mkv_handler.hpp"
#include "../containers/docx_handler.hpp"
#include "../containers/xlsx_handler.hpp"
#include "../containers/pptx_handler.hpp"

namespace fs = std::filesystem;

// static factory for handlers
static std::unique_ptr<IContainer> make_handler(const ContainerFormat fmt) {
    switch (fmt) {
        case ContainerFormat::Mkv:   return std::make_unique<MkvHandler>();
        case ContainerFormat::Docx:  return std::make_unique<DocxHandler>();
        case ContainerFormat::Xlsx:  return std::make_unique<XlsxHandler>();
        case ContainerFormat::Pptx:  return std::make_unique<PptxHandler>();
        default:                     return std::make_unique<ArchiveHandler>();
    }
}

void collect_inputs(const std::vector<fs::path>& inputs,
                    bool recursive,
                    std::vector<fs::path>& files,
                    std::vector<ContainerJob>& archive_jobs, Settings &settings) {
    for (auto const& p : inputs) {
        if (p == "-") {
            fs::path tmp = make_temp_path("stdin", ".bin");
            std::ofstream out(tmp, std::ios::binary);
            out << std::cin.rdbuf();
            out.close();
            files.push_back(tmp);
            settings.is_pipe = true;
            continue;
        }
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
            continue;
        }

        // skip junk
        auto lower = p.filename().string();
        std::ranges::transform(lower, lower.begin(), ::tolower);
        if (lower == ".ds_store" || lower == "desktop.ini") continue;

        // detect format
        auto mime = detect_mime_type(p.string());
        auto fmt_it = mime_to_format.find(mime);
        if (fmt_it != mime_to_format.end() && can_read_format(fmt_it->second)) {
            auto handler = make_handler(fmt_it->second);
            auto job = handler->prepare(p.string());
            if (!job.file_list.empty() || !job.children.empty()) {
                archive_jobs.push_back(job);
                for (const auto& f : job.file_list) {
                    files.emplace_back(f);
                }
            }
        } else {
            files.push_back(p);
        }
    }

    Logger::log(LogLevel::INFO,
                "Scanner collected " + std::to_string(files.size()) +
                " files and " + std::to_string(archive_jobs.size()) +
                " container jobs",
                "file_scanner");
}