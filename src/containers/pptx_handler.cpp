//
// Created by Giuseppe Francione on 05/10/25.
//

#include "pptx_handler.hpp"
#include "../utils/logger.hpp"
#include "../utils/file_type.hpp"
#include "../utils/archive_formats.hpp"
#include "../containers/archive_handler.hpp"
#include <archive.h>
#include <archive_entry.h>
#include "zopfli.h"
#include "zlib_container.h"
#include <vector>
#include <fstream>
#include <system_error>
#include <filesystem>
#include <random>

// helper: recompress with zopfli
static std::vector<unsigned char> recompress_with_zopfli(const std::vector<unsigned char> &input) {
    ZopfliOptions opts;
    ZopfliInitOptions(&opts);
    opts.numiterations = 15;
    opts.blocksplitting = 1;

    unsigned char *out_data = nullptr;
    size_t out_size = 0;
    ZopfliZlibCompress(&opts, input.data(), input.size(), &out_data, &out_size);

    std::vector<unsigned char> result(out_data, out_data + out_size);
    free(out_data);
    return result;
}

static ContainerFormat mime_to_container_format(const std::string &mime) {
    auto it = mime_to_format.find(mime);
    if (it != mime_to_format.end()) return it->second;
    return ContainerFormat::Unknown;
}

ContainerJob PptxHandler::prepare(const std::string &path) {
    Logger::log(LogLevel::INFO, "Preparing PPTX container: " + path, "PptxHandler");

    ContainerJob job;
    job.original_path = path;
    job.format = ContainerFormat::Pptx;

    // temp dir
    thread_local static std::mt19937_64 rng{std::random_device{}()};
    thread_local static std::uniform_int_distribution<unsigned long long> dist;
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path() / ("pptx_" + std::to_string(dist(rng)));
    std::filesystem::create_directories(temp_dir);
    job.temp_dir = temp_dir.string();

    archive *in = archive_read_new();
    archive_read_support_format_zip(in);
    if (archive_read_open_filename(in, path.c_str(), 10240) != ARCHIVE_OK) {
        Logger::log(LogLevel::ERROR, "Failed to open PPTX for reading", "PptxHandler");
        archive_read_free(in);
        return job;
    }

    archive_entry *entry;
    while (archive_read_next_header(in, &entry) == ARCHIVE_OK) {
        std::string name = archive_entry_pathname(entry);
        std::filesystem::path out_path = temp_dir / name;
        std::filesystem::create_directories(out_path.parent_path());

        std::ofstream ofs(out_path, std::ios::binary);
        const void *buff;
        size_t size;
        la_int64_t offset;
        while (archive_read_data_block(in, &buff, &size, &offset) == ARCHIVE_OK) {
            ofs.write(reinterpret_cast<const char *>(buff), size);
        }
        ofs.close();

        const std::string mime = detect_mime_type(out_path.string());
        const ContainerFormat fmt = mime_to_container_format(mime);
        if (fmt != ContainerFormat::Unknown && can_read_format(fmt)) {
            Logger::log(LogLevel::DEBUG, "Found nested container in PPTX: " + out_path.string(), "PptxHandler");
            if (fmt == ContainerFormat::Pptx) {
                PptxHandler nested;
                job.children.push_back(nested.prepare(out_path.string()));
            } else {
                ArchiveHandler ah;
                job.children.push_back(ah.prepare(out_path.string()));
            }
        } else {
            job.file_list.push_back(out_path.string());
        }
    }

    archive_read_close(in);
    archive_read_free(in);

    Logger::log(LogLevel::DEBUG, "PPTX prepare complete: " + std::to_string(job.file_list.size()) +
                                 " files, " + std::to_string(job.children.size()) + " nested containers",
                "PptxHandler");

    return job;
}

bool PptxHandler::finalize(const ContainerJob &job, Settings &settings) {
    Logger::log(LogLevel::INFO, "Finalizing PPTX container: " + job.original_path, "PptxHandler");

    namespace fs = std::filesystem;
    std::error_code ec;

    for (const auto &child: job.children) {
        bool ok = false;
        if (child.format == ContainerFormat::Pptx) {
            PptxHandler nested;
            ok = nested.finalize(child, settings);
        } else {
            ArchiveHandler ah;
            ok = ah.finalize(child, settings);
        }
        if (!ok) return false;
    }

    fs::path src_path(job.original_path);
    fs::path tmp_path = src_path.parent_path() / (src_path.stem().string() + "_tmp.pptx");

    struct archive *out = archive_write_new();
    archive_write_set_format_zip(out);
    if (archive_write_open_filename(out, tmp_path.string().c_str()) != ARCHIVE_OK) {
        Logger::log(LogLevel::ERROR, "Failed to open temp PPTX for writing", "PptxHandler");
        archive_write_free(out);
        return false;
    }

    for (const auto &file: job.file_list) {
        fs::path rel = fs::relative(file, job.temp_dir, ec);
        if (ec) rel = fs::path(file).filename();

        std::ifstream ifs(file, std::ios::binary);
        std::vector<unsigned char> buf((std::istreambuf_iterator<char>(ifs)), {});

        std::vector<unsigned char> final_data;
        if (rel.extension() == ".xml" || rel.extension() == ".rels") {
            final_data = recompress_with_zopfli(buf);
        } else {
            final_data = buf;
        }

        archive_entry *entry = archive_entry_new();
        archive_entry_set_pathname(entry, rel.generic_string().c_str());
        archive_entry_set_size(entry, final_data.size());
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0644);
        archive_entry_set_mtime(entry, 0, 0);

        archive_write_header(out, entry);
        archive_write_data(out, final_data.data(), final_data.size());
        archive_entry_free(entry);
    }

    archive_write_close(out);
    archive_write_free(out);

    auto orig_size = fs::file_size(src_path, ec);
    auto new_size = fs::file_size(tmp_path, ec);
    if (new_size > 0 && (orig_size == 0 || new_size < orig_size)) {
        fs::rename(tmp_path, src_path, ec);
        Logger::log(LogLevel::INFO, "Optimized PPTX: " + src_path.string(), "PptxHandler");
    } else {
        fs::remove(tmp_path, ec);
        Logger::log(LogLevel::DEBUG, "No improvement for: " + src_path.string(), "PptxHandler");
    }

    fs::remove_all(job.temp_dir, ec);
    return true;
}
