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
#include "ooxml_handler.hpp"
#include "../encoder/zopflipng_encoder.hpp"
#include "../utils/random_utils.hpp"

using namespace std;

const char* OdfHandler::temp_prefix() const {
    switch (fmt_) {
        case ContainerFormat::Odt: return "odt_";
        case ContainerFormat::Ods: return "ods_";
        case ContainerFormat::Odp: return "odp_";
        default: return "odf_";
    }
}

const char* OdfHandler::output_extension() const {
    switch (fmt_) {
        case ContainerFormat::Odt: return ".odt";
        case ContainerFormat::Ods: return ".ods";
        case ContainerFormat::Odp: return ".odp";
        default: return ".odf";
    }
}

OdfHandler OdfHandler::from_path(const string& path) {
    filesystem::path p(path);
    string ext = p.extension().string();
    if (ext == ".odt") return OdfHandler(ContainerFormat::Odt);
    if (ext == ".ods") return OdfHandler(ContainerFormat::Ods);
    if (ext == ".odp") return OdfHandler(ContainerFormat::Odp);
    return OdfHandler(ContainerFormat::Unknown);
}

ContainerJob OdfHandler::prepare(const string& path) {
    const char* handler_name =
        (fmt_ == ContainerFormat::Odt ? "OdfHandler(ODT)" :
        (fmt_ == ContainerFormat::Ods ? "OdfHandler(ODS)" :
        (fmt_ == ContainerFormat::Odp ? "OdfHandler(ODP)" : "OdfHandler")));

    Logger::log(LogLevel::INFO, string("Preparing ODF container: ") + path, handler_name);

    ContainerJob job;
    job.original_path = path;
    job.format = fmt_;

    filesystem::path temp_dir = filesystem::temp_directory_path() / (string(temp_prefix()) + RandomUtils::random_suffix());
    filesystem::create_directories(temp_dir);
    job.temp_dir = temp_dir.string();

    archive* in = archive_read_new();
    archive_read_support_format_zip(in);

    int open_r = archive_read_open_filename(in, path.c_str(), 10240);
    if (open_r != ARCHIVE_OK && open_r != ARCHIVE_WARN) {
        Logger::log(LogLevel::ERROR, string("Failed to open ODF for reading: ") + archive_error_string(in), handler_name);
        archive_read_free(in);
        return job;
    }
    if (open_r == ARCHIVE_WARN) {
        Logger::log(LogLevel::WARNING, string("LIBARCHIVE WARN: ") + archive_error_string(in), handler_name);
    }

    archive_entry* entry = nullptr;
    int r = ARCHIVE_OK;

    while ((r = archive_read_next_header(in, &entry)) == ARCHIVE_OK) {
        const char* ename = archive_entry_pathname(entry);
        if (!ename) {
            Logger::log(LogLevel::WARNING, "Entry with null name skipped", handler_name);
            archive_read_data_skip(in);
            continue;
        }

        string name = ename;
        filesystem::path out_path = temp_dir / name;

        error_code ec;
        filesystem::create_directories(out_path.parent_path(), ec);
        if (ec) {
            Logger::log(LogLevel::ERROR,
                        "Failed to create parent dir: " + out_path.parent_path().string() + " (" + ec.message() + ")",
                        handler_name);
            archive_read_data_skip(in);
            continue;
        }

        ofstream ofs(out_path, ios::binary);
        if (!ofs) {
            Logger::log(LogLevel::ERROR, "Failed to create file during extraction: " + out_path.string(), handler_name);
            archive_read_data_skip(in);
            continue;
        }

        const void* buff = nullptr;
        size_t size = 0;
        la_int64_t offset = 0;
        for (;;) {
            int rb = archive_read_data_block(in, &buff, &size, &offset);
            if (rb == ARCHIVE_EOF) break;
            if (rb != ARCHIVE_OK) {
                Logger::log(LogLevel::ERROR, string("Error reading data block: ") + archive_error_string(in), handler_name);
                break;
            }
            ofs.write(reinterpret_cast<const char*>(buff), static_cast<std::streamsize>(size));
        }
        ofs.close();

        // Nested container detection
        const string mime = detect_mime_type(out_path.string());
        auto it = mime_to_format.find(mime);
        ContainerFormat inner_fmt = (it != mime_to_format.end()) ? it->second : ContainerFormat::Unknown;

        if (inner_fmt != ContainerFormat::Unknown && can_read_format(inner_fmt)) {
            Logger::log(LogLevel::DEBUG, "Found nested container: " + out_path.string() + " (" + mime + ")", handler_name);

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
        Logger::log(LogLevel::ERROR, string("Iteration error: ") + archive_error_string(in), handler_name);
    }

    archive_read_close(in);
    archive_read_free(in);

    Logger::log(LogLevel::DEBUG,
                string("ODF prepare complete: ") +
                to_string(job.file_list.size()) + " files, " +
                to_string(job.children.size()) + " nested containers",
                handler_name);

    return job;
}

bool OdfHandler::finalize(const ContainerJob& job, Settings& settings) {
    const char* handler_name =
        (fmt_ == ContainerFormat::Odt ? "OdfHandler(ODT)" :
        (fmt_ == ContainerFormat::Ods ? "OdfHandler(ODS)" :
        (fmt_ == ContainerFormat::Odp ? "OdfHandler(ODP)" : "OdfHandler")));

    Logger::log(LogLevel::INFO, "Finalizing ODF container: " + job.original_path, handler_name);

    namespace fs = std::filesystem;
    std::error_code ec;

    for (const auto& child : job.children) {
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

    struct archive* out = archive_write_new();
    if (!out) {
        Logger::log(LogLevel::ERROR, "archive_write_new failed", handler_name);
        return false;
    }

    int set_fmt = archive_write_set_format_zip(out);
    if (set_fmt == ARCHIVE_WARN) {
        Logger::log(LogLevel::WARNING, string("LIBARCHIVE WARN: ") + archive_error_string(out), handler_name);
    }
    if (set_fmt != ARCHIVE_OK) {
        Logger::log(LogLevel::ERROR, string("Failed to set ZIP format: ") + archive_error_string(out), handler_name);
        archive_write_free(out);
        return false;
    }

int open_w = archive_write_open_filename(out, tmp_path.string().c_str());
    if (open_w == ARCHIVE_WARN) {
        Logger::log(LogLevel::WARNING, string("LIBARCHIVE WARN: ") + archive_error_string(out), handler_name);
    }
    if (open_w != ARCHIVE_OK) {
        Logger::log(LogLevel::ERROR, string("Failed to open temp ODF for writing: ") + archive_error_string(out), handler_name);
        archive_write_free(out);
        return false;
    }

    for (const auto& file : job.file_list) {
        fs::path rel = fs::relative(file, job.temp_dir, ec);
        if (ec) rel = fs::path(file).filename();

        ifstream ifs(file, ios::binary);
        if (!ifs) {
            Logger::log(LogLevel::ERROR, "Failed to open file for reading: " + file, handler_name);
            continue;
        }

        vector<unsigned char> buf((istreambuf_iterator<char>(ifs)), istreambuf_iterator<char>());

        // Per ODF, ricomprimiamo solo i file XML e RELS, come per OOXML
        vector<unsigned char> final_data;
        string ext = rel.extension().string();
        if (ext == ".xml" || ext == ".rels") {
            final_data = ZopfliPngEncoder::recompress_with_zopfli(buf);
            Logger::log(LogLevel::DEBUG,
                        "Recompressed entry: " + rel.string() +
                        " (" + to_string(buf.size()) + " -> " + to_string(final_data.size()) + " bytes)",
                        handler_name);
        } else {
            final_data = buf;
            Logger::log(LogLevel::DEBUG, "Copied entry without recompression: " + rel.string(), handler_name);
        }

        archive_entry* entry = archive_entry_new();
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
        archive_entry_set_mtime(entry, 0, 0); // determinismo

        int wh = archive_write_header(out, entry);
        if (wh == ARCHIVE_WARN) {
            Logger::log(LogLevel::WARNING, string("LIBARCHIVE WARN: ") + archive_error_string(out), handler_name);
        }
        if (wh != ARCHIVE_OK) {
            Logger::log(LogLevel::ERROR,
                        "Failed to write header for: " + rel.string() +
                        " (" + string(archive_error_string(out)) + ")",
                        handler_name);
            archive_entry_free(entry);
            archive_write_close(out);
            archive_write_free(out);
            return false;
        }

        la_ssize_t wrote = archive_write_data(out, final_data.data(), final_data.size());
        if (wrote < 0) {
            Logger::log(LogLevel::ERROR,
                        "Failed to write data for: " + rel.string() +
                        " (" + string(archive_error_string(out)) + ")",
                        handler_name);
            archive_entry_free(entry);
            archive_write_close(out);
            archive_write_free(out);
            return false;
        }

        archive_entry_free(entry);
    }

    int close_w = archive_write_close(out);
    if (close_w != ARCHIVE_OK) {
        Logger::log(LogLevel::ERROR, string("Failed to close archive: ") + archive_error_string(out), handler_name);
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
                    " (" + to_string(orig_size) + " -> " + to_string(new_size) + " bytes)",
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