//
// Created by Giuseppe Francione on 04/10/25.
//

#include "docx_handler.hpp"
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

// helper: map mime to containerformat using provided tables
static ContainerFormat mime_to_container_format(const std::string &mime) {
    auto it = mime_to_format.find(mime);
    if (it != mime_to_format.end()) {
        return it->second;
    }
    return ContainerFormat::Unknown;
}

ContainerJob DocxHandler::prepare(const std::string &path) {
    Logger::log(LogLevel::INFO, "Preparing DOCX handler: " + path, "DocxHandler");

    ContainerJob job;
    job.original_path = path;
    job.format = ContainerFormat::Docx;

    // create temp dir
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path() / ("docx_" + std::to_string(std::rand()));
    std::filesystem::create_directories(temp_dir);
    job.temp_dir = temp_dir.string();

    archive *in = archive_read_new();
    archive_read_support_format_zip(in);
    int open_r = archive_read_open_filename(in, path.c_str(), 10240);
    if (open_r != ARCHIVE_OK && open_r != ARCHIVE_WARN) {
        Logger::log(LogLevel::ERROR, "Failed to open DOCX for reading: " + std::string(archive_error_string(in)),
                    "DocxHandler");
        archive_read_free(in);
        return job;
    }
    if (open_r == ARCHIVE_WARN) {
        Logger::log(LogLevel::WARNING, std::string("LIBARCHIVE WARN: ") + archive_error_string(in), "DocxHandler");
    }

    archive_entry *entry;
    int r = ARCHIVE_OK;
    while ((r = archive_read_next_header(in, &entry)) == ARCHIVE_OK) {
        const char *ename = archive_entry_pathname(entry);
        if (!ename) {
            Logger::log(LogLevel::WARNING, "Entry with null name skipped", "DocxHandler");
            archive_read_data_skip(in);
            continue;
        }

        std::string name = ename;
        std::filesystem::path out_path = temp_dir / name;
        std::error_code ec;
        std::filesystem::create_directories(out_path.parent_path(), ec);
        if (ec) {
            Logger::log(LogLevel::ERROR,
                        "Failed to create parent dir: " + out_path.parent_path().string() + " (" + ec.message() + ")",
                        "DocxHandler");
            archive_read_data_skip(in);
            continue;
        }

        std::ofstream ofs(out_path, std::ios::binary);
        if (!ofs) {
            Logger::log(LogLevel::ERROR, "Failed to create file during extraction: " + out_path.string(),
                        "DocxHandler");
            archive_read_data_skip(in);
            continue;
        }

        const void *buff = nullptr;
        size_t size = 0;
        la_int64_t offset = 0;
        while (true) {
            int rb = archive_read_data_block(in, &buff, &size, &offset);
            if (rb == ARCHIVE_EOF) break;
            if (rb != ARCHIVE_OK) {
                Logger::log(LogLevel::ERROR, "Error reading data block: " + std::string(archive_error_string(in)),
                            "DocxHandler");
                break;
            }
            ofs.write(reinterpret_cast<const char *>(buff), static_cast<std::streamsize>(size));
        }
        ofs.close();

        // decide if this entry is itself a container using mime detection (no is_archive_file here)
        const std::string mime = detect_mime_type(out_path.string());
        const ContainerFormat fmt = mime_to_container_format(mime);

        if (fmt != ContainerFormat::Unknown && can_read_format(fmt)) {
            Logger::log(LogLevel::DEBUG, "Found nested container in DOCX: " + out_path.string() + " (" + mime + ")",
                        "DocxHandler");
            // delegate to appropriate container handler
            if (fmt == ContainerFormat::Docx) {
                DocxHandler nested;
                job.children.push_back(nested.prepare(out_path.string()));
            } else {
                // generic archives or other containers handled by archivehandler
                ArchiveHandler ah;
                job.children.push_back(ah.prepare(out_path.string()));
            }
        } else {
            job.file_list.push_back(out_path.string());
        }
    }

    if (r != ARCHIVE_EOF) {
        Logger::log(LogLevel::ERROR, "Iteration error: " + std::string(archive_error_string(in)), "DocxHandler");
    }

    archive_read_close(in);
    archive_read_free(in);

    Logger::log(LogLevel::DEBUG, "DOCX prepare complete: " + std::to_string(job.file_list.size()) +
                                 " files, " + std::to_string(job.children.size()) + " nested containers",
                "DocxHandler");

    return job;
}

bool DocxHandler::finalize(const ContainerJob &job, Settings &settings) {
    Logger::log(LogLevel::INFO, "Finalizing DOCX container: " + job.original_path, "DocxHandler");

    namespace fs = std::filesystem;
    std::error_code ec;

    // finalize children first with coherence to formats
    for (const auto &child: job.children) {
        bool child_ok = false;
        if (child.format == ContainerFormat::Docx) {
            DocxHandler nested;
            child_ok = nested.finalize(child, settings);
        } else {
            ArchiveHandler ah;
            child_ok = ah.finalize(child, settings);
        }
        if (!child_ok) {
            Logger::log(LogLevel::ERROR, "Failed to finalize nested container: " + child.original_path, "DocxHandler");
            return false;
        }
    }

    fs::path src_path(job.original_path);
    fs::path tmp_path = src_path.parent_path() / (src_path.stem().string() + "_tmp.docx");

    struct archive *out = archive_write_new();
    if (!out) {
        Logger::log(LogLevel::ERROR, "archive_write_new failed", "DocxHandler");
        return false;
    }
    int set_fmt = archive_write_set_format_zip(out);
    if (set_fmt == ARCHIVE_WARN) {
        Logger::log(LogLevel::WARNING, std::string("LIBARCHIVE WARN: ") + archive_error_string(out), "DocxHandler");
    }
    if (set_fmt != ARCHIVE_OK) {
        Logger::log(LogLevel::ERROR, "Failed to set ZIP format: " + std::string(archive_error_string(out)),
                    "DocxHandler");
        archive_write_free(out);
        return false;
    }
    // set deflate compression for zip entries
    int open_w = archive_write_open_filename(out, tmp_path.string().c_str());
    if (open_w == ARCHIVE_WARN) {
        Logger::log(LogLevel::WARNING, std::string("LIBARCHIVE WARN: ") + archive_error_string(out), "DocxHandler");
    }
    if (open_w != ARCHIVE_OK) {
        Logger::log(LogLevel::ERROR, "Failed to open temp DOCX for writing: " + std::string(archive_error_string(out)),
                    "DocxHandler");
        archive_write_free(out);
        return false;
    }

    for (const auto &file: job.file_list) {
        fs::path rel = fs::relative(file, job.temp_dir, ec);
        if (ec) rel = fs::path(file).filename();

        std::ifstream ifs(file, std::ios::binary);
        if (!ifs) {
            Logger::log(LogLevel::ERROR, "Failed to open file for reading: " + file, "DocxHandler");
            // continue to next entry; we keep building the archive
            continue;
        }
        std::vector<unsigned char> buf((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());

        std::vector<unsigned char> final_data;
        const auto ext = rel.extension().string();
        if (ext == ".xml" || ext == ".rels") {
            final_data = recompress_with_zopfli(buf);
            Logger::log(LogLevel::DEBUG,
                        "Recompressed entry: " + rel.string() + " (" + std::to_string(buf.size()) + " -> " +
                        std::to_string(final_data.size()) + " bytes)", "DocxHandler");
        } else {
            final_data = buf;
            Logger::log(LogLevel::DEBUG, "Copied entry without recompression: " + rel.string(), "DocxHandler");
        }

        archive_entry *entry = archive_entry_new();
        if (!entry) {
            Logger::log(LogLevel::ERROR, "archive_entry_new failed", "DocxHandler");
            archive_write_close(out);
            archive_write_free(out);
            return false;
        }
        archive_entry_set_pathname(entry, rel.generic_string().c_str());
        archive_entry_set_size(entry, static_cast<la_int64_t>(final_data.size()));
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0644);
        // determinism: zero timestamps
        archive_entry_set_mtime(entry, 0, 0);

        int wh = archive_write_header(out, entry);
        if (wh == ARCHIVE_WARN) {
            Logger::log(LogLevel::WARNING, std::string("LIBARCHIVE WARN: ") + archive_error_string(out), "DocxHandler");
        }
        if (wh != ARCHIVE_OK) {
            Logger::log(LogLevel::ERROR,
                        "Failed to write header for: " + rel.string() + " (" + std::string(archive_error_string(out)) +
                        ")", "DocxHandler");
            archive_entry_free(entry);
            archive_write_close(out);
            archive_write_free(out);
            return false;
        }

        la_ssize_t wrote = archive_write_data(out, final_data.data(), final_data.size());
        if (wrote < 0) {
            Logger::log(LogLevel::ERROR,
                        "Failed to write data for: " + rel.string() + " (" + std::string(archive_error_string(out)) +
                        ")", "DocxHandler");
            archive_entry_free(entry);
            archive_write_close(out);
            archive_write_free(out);
            return false;
        }

        archive_entry_free(entry);
    }

    int close_w = archive_write_close(out);
    if (close_w != ARCHIVE_OK) {
        Logger::log(LogLevel::ERROR, "Failed to close archive: " + std::string(archive_error_string(out)),
                    "DocxHandler");
        archive_write_free(out);
        return false;
    }
    archive_write_free(out);

    auto orig_size = fs::file_size(src_path, ec);
    if (ec) orig_size = 0;
    auto new_size = fs::file_size(tmp_path, ec);
    if (ec) new_size = 0;

    if (new_size > 0 && (orig_size == 0 || new_size < orig_size)) {
        fs::rename(tmp_path, src_path, ec);
        if (ec) {
            Logger::log(LogLevel::ERROR, "Failed to replace original DOCX: " + ec.message(), "DocxHandler");
            return false;
        }
        Logger::log(LogLevel::INFO,
                    "Optimized DOCX: " + src_path.string() +
                    " (" + std::to_string(orig_size) + " -> " + std::to_string(new_size) + " bytes)",
                    "DocxHandler");
    } else {
        fs::remove(tmp_path, ec);
        Logger::log(LogLevel::DEBUG, "No improvement for: " + src_path.string(), "DocxHandler");
    }

    // cleanup temp dir
    fs::remove_all(job.temp_dir, ec);
    if (ec) {
        Logger::log(LogLevel::WARNING, "Can't remove temp dir: " + job.temp_dir + " (" + ec.message() + ")",
                    "DocxHandler");
    } else {
        Logger::log(LogLevel::DEBUG, "Removed temp dir: " + job.temp_dir, "DocxHandler");
    }

    return true;
}
