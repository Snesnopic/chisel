//
// Created by Giuseppe Francione on 06/10/25.
//

#include "odf_handler.hpp"
#include "../utils/logger.hpp"
#include "../utils/file_type.hpp"
#include "../utils/archive_formats.hpp"
#include "../containers/archive_handler.hpp"
#include <archive.h>
#include <archive_entry.h>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>
#include "../encoder/zopflipng_encoder.hpp"
#include "../utils/random_utils.hpp"

static const char *handler_tag_for(ContainerFormat fmt) {
    switch (fmt) {
        case ContainerFormat::Odt: return "OdfHandler(ODT)";
        case ContainerFormat::Ods: return "OdfHandler(ODS)";
        case ContainerFormat::Odp: return "OdfHandler(ODP)";
        case ContainerFormat::Odg: return "OdfHandler(ODG)";
        case ContainerFormat::Odf: return "OdfHandler(ODF)";
        default: return "OdfHandler";
    }
}

const char *OdfHandler::temp_prefix() const {
    switch (fmt_) {
        case ContainerFormat::Odt: return "odt_";
        case ContainerFormat::Ods: return "ods_";
        case ContainerFormat::Odp: return "odp_";
        case ContainerFormat::Odg: return "odg_";
        case ContainerFormat::Odf: return "odf_";
        default: return "odf_";
    }
}

const char *OdfHandler::output_extension() const {
    switch (fmt_) {
        case ContainerFormat::Odt: return ".odt";
        case ContainerFormat::Ods: return ".ods";
        case ContainerFormat::Odp: return ".odp";
        case ContainerFormat::Odg: return ".odg";
        case ContainerFormat::Odf: return ".odf";
        default: return ".odf";
    }
}

OdfHandler OdfHandler::from_path(const std::string &path) {
    std::filesystem::path p(path);
    std::string ext = p.extension().string();
    if (ext == ".odt") return OdfHandler(ContainerFormat::Odt);
    if (ext == ".ods") return OdfHandler(ContainerFormat::Ods);
    if (ext == ".odp") return OdfHandler(ContainerFormat::Odp);
    if (ext == ".odg") return OdfHandler(ContainerFormat::Odg);
    if (ext == ".odf") return OdfHandler(ContainerFormat::Odf);
    return OdfHandler(ContainerFormat::Unknown);
}

ContainerJob OdfHandler::prepare(const std::string &path) {
    const char *handler_name = handler_tag_for(fmt_);
    Logger::log(LogLevel::INFO, std::string("Preparing ODF container: ") + path, handler_name);

    ContainerJob job;
    job.original_path = path;
    job.format = fmt_;

    std::filesystem::path temp_dir = std::filesystem::temp_directory_path() / (
                                    std::string(temp_prefix()) + RandomUtils::random_suffix());
    std::filesystem::create_directories(temp_dir);
    job.temp_dir = temp_dir.string();

    archive *in = archive_read_new();
    archive_read_support_format_zip(in);

    int open_r = archive_read_open_filename(in, path.c_str(), 10240);
    if (open_r != ARCHIVE_OK && open_r != ARCHIVE_WARN) {
        Logger::log(LogLevel::ERROR, std::string("Failed to open ODF for reading: ") + archive_error_string(in),
                    handler_name);
        archive_read_free(in);
        return job;
    }
    if (open_r == ARCHIVE_WARN) {
        Logger::log(LogLevel::WARNING, std::string("LIBARCHIVE WARN: ") + archive_error_string(in), handler_name);
    }

    archive_entry *entry = nullptr;
    int r = ARCHIVE_OK;

    while ((r = archive_read_next_header(in, &entry)) == ARCHIVE_OK) {
        const char *ename = archive_entry_pathname(entry);
        if (!ename) {
            Logger::log(LogLevel::WARNING, "Entry with null name skipped", handler_name);
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
                        handler_name);
            archive_read_data_skip(in);
            continue;
        }

        std::ofstream ofs(out_path, std::ios::binary);
        if (!ofs) {
            Logger::log(LogLevel::ERROR, "Failed to create file during extraction: " + out_path.string(), handler_name);
            archive_read_data_skip(in);
            continue;
        }

        const void *buff = nullptr;
        size_t size = 0;
        la_int64_t offset = 0;
        for (;;) {
            int rb = archive_read_data_block(in, &buff, &size, &offset);
            if (rb == ARCHIVE_EOF) break;
            if (rb != ARCHIVE_OK) {
                Logger::log(LogLevel::ERROR, std::string("Error reading data block: ") + archive_error_string(in),
                            handler_name);
                break;
            }
            ofs.write(reinterpret_cast<const char *>(buff), static_cast<std::streamsize>(size));
        }
        ofs.close();

        // nested container detection
        const std::string mime = detect_mime_type(out_path.string());
        auto it = mime_to_format.find(mime);
        ContainerFormat inner_fmt = (it != mime_to_format.end()) ? it->second : ContainerFormat::Unknown;

        if (inner_fmt != ContainerFormat::Unknown && can_read_format(inner_fmt)) {
            Logger::log(LogLevel::DEBUG, "Found nested container: " + out_path.string() + " (" + mime + ")",
                        handler_name);

            if (inner_fmt == fmt_) {
                OdfHandler nested(fmt_);
                job.children.push_back(nested.prepare(out_path.string()));
            } else {
                ArchiveHandler ah;
                job.children.push_back(ah.prepare(out_path.string()));
            }
        } else {
            job.file_list.push_back(out_path.string());
        }
    }

    if (r != ARCHIVE_EOF) {
        Logger::log(LogLevel::ERROR, std::string("Iteration error: ") + archive_error_string(in), handler_name);
    }

    archive_read_close(in);
    archive_read_free(in);

    Logger::log(LogLevel::DEBUG,
                std::string("ODF prepare complete: ") +
                std::to_string(job.file_list.size()) + " files, " +
                std::to_string(job.children.size()) + " nested containers",
                handler_name);

    return job;
}

bool OdfHandler::finalize(const ContainerJob &job, Settings &settings) {
    const char *handler_name = handler_tag_for(fmt_);
    Logger::log(LogLevel::INFO, "Finalizing ODF container: " + job.original_path, handler_name);

    namespace fs = std::filesystem;
    std::error_code ec;

    // finalize children first
    for (const auto &child: job.children) {
        bool ok = false;
        if (child.format == fmt_) {
            OdfHandler nested(fmt_);
            ok = nested.finalize(child, settings);
        } else {
            ArchiveHandler ah;
            ok = ah.finalize(child, settings);
        }
        if (!ok) {
            Logger::log(LogLevel::ERROR, "Failed to finalize nested container: " + child.original_path, handler_name);
            return false;
        }
    }

    fs::path src_path(job.original_path);
    fs::path tmp_path = src_path.parent_path() / (src_path.stem().string() + "_tmp" + output_extension());

    struct archive *out = archive_write_new();
    if (!out) {
        Logger::log(LogLevel::ERROR, "archive_write_new failed", handler_name);
        return false;
    }

    // set ZIP format and force deflate compression
    int set_fmt = archive_write_set_format_zip(out);
    if (set_fmt == ARCHIVE_WARN) {
        Logger::log(LogLevel::WARNING, std::string("LIBARCHIVE WARN: ") + archive_error_string(out), handler_name);
    }
    if (set_fmt != ARCHIVE_OK) {
        Logger::log(LogLevel::ERROR, "Failed to set ZIP format: " + std::string(archive_error_string(out)), handler_name);
        archive_write_free(out);
        return false;
    }
    archive_write_set_options(out, "compression=deflate");

    int open_w = archive_write_open_filename(out, tmp_path.string().c_str());
    if (open_w == ARCHIVE_WARN) {
        Logger::log(LogLevel::WARNING, std::string("LIBARCHIVE WARN: ") + archive_error_string(out), handler_name);
    }
    if (open_w != ARCHIVE_OK) {
        Logger::log(LogLevel::ERROR, "Failed to open temp ODF for writing: " + std::string(archive_error_string(out)), handler_name);
        archive_write_free(out);
        return false;
    }

    // ensure "mimetype" is written first
    std::vector<std::string> files_ordered;
    auto it = std::find_if(job.file_list.begin(), job.file_list.end(),
                           [](const std::string &f){ return fs::path(f).filename() == "mimetype"; });
    if (it != job.file_list.end()) {
        files_ordered.push_back(*it);
    }
    for (const auto &f : job.file_list) {
        if (fs::path(f).filename() != "mimetype") {
            files_ordered.push_back(f);
        }
    }

    // write all entries
    for (const auto &file: files_ordered) {
        fs::path rel = fs::relative(file, job.temp_dir, ec);
        if (ec) rel = fs::path(file).filename();

        std::ifstream ifs(file, std::ios::binary);
        if (!ifs) {
            Logger::log(LogLevel::ERROR, "Failed to open file for reading: " + file, handler_name);
            continue;
        }
        std::vector<unsigned char> buf((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());

        std::vector<unsigned char> final_data;
        std::string ext = rel.extension().string();

        if (rel.filename() == "mimetype") {
            // mimetype: must be stored uncompressed
            final_data = buf;
            Logger::log(LogLevel::DEBUG, "Stored mimetype entry uncompressed", handler_name);
        } else if (ext == ".xml") {
            final_data = ZopfliPngEncoder::recompress_with_zopfli(buf);
            Logger::log(LogLevel::DEBUG, "Recompressed XML with Zopfli: " + rel.string(), handler_name);
        } else {
            final_data = buf;
            Logger::log(LogLevel::DEBUG, "Copied entry unchanged: " + rel.string(), handler_name);
        }

        archive_entry *entry = archive_entry_new();
        if (!entry) {
            Logger::log(LogLevel::ERROR, "archive_entry_new failed", handler_name);
            archive_write_close(out);
            archive_write_free(out);
            return false;
        }

        archive_entry_set_pathname(entry, rel.generic_string().c_str());
        archive_entry_set_size(entry, static_cast<la_int64_t>(final_data.size()));
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0644);
        archive_entry_set_mtime(entry, 0, 0); // determinism

        if (rel.filename() == "mimetype") {
            // force store (no compression)
            archive_write_set_options(out, "compression=store");
        } else {
            archive_write_set_options(out, "compression=deflate");
        }

        int wh = archive_write_header(out, entry);
        if (wh == ARCHIVE_WARN) {
            Logger::log(LogLevel::WARNING, std::string("LIBARCHIVE WARN: ") + archive_error_string(out), handler_name);
        }
        if (wh != ARCHIVE_OK) {
            Logger::log(LogLevel::ERROR,
                        "Failed to write header for: " + rel.string() +
                        " (" + std::string(archive_error_string(out)) + ")", handler_name);
            archive_entry_free(entry);
            archive_write_close(out);
            archive_write_free(out);
            return false;
        }

        la_ssize_t wrote = archive_write_data(out, final_data.data(), final_data.size());
        if (wrote < 0) {
            Logger::log(LogLevel::ERROR,
                        "Failed to write data for: " + rel.string() +
                        " (" + std::string(archive_error_string(out)) + ")", handler_name);
            archive_entry_free(entry);
            archive_write_close(out);
            archive_write_free(out);
            return false;
        }

        archive_entry_free(entry);
    }

    int close_w = archive_write_close(out);
    if (close_w != ARCHIVE_OK) {
        Logger::log(LogLevel::ERROR, "Failed to close archive: " + std::string(archive_error_string(out)), handler_name);
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
            Logger::log(LogLevel::ERROR, "Failed to replace original ODF: " + ec.message(), handler_name);
            return false;
        }
        Logger::log(LogLevel::INFO,
                    "Optimized ODF: " + src_path.string() +
                    " (" + std::to_string(orig_size) + " -> " + std::to_string(new_size) + " bytes)",
                    handler_name);
    } else {
        fs::remove(tmp_path, ec);
        Logger::log(LogLevel::DEBUG, "No improvement for: " + src_path.string(), handler_name);
    }

    fs::remove_all(job.temp_dir, ec);
    if (ec) {
        Logger::log(LogLevel::WARNING, "Can't remove temp dir: " + job.temp_dir + " (" + ec.message() + ")", handler_name);
    } else {
        Logger::log(LogLevel::DEBUG, "Removed temp dir: " + job.temp_dir, handler_name);
    }

    return true;
}