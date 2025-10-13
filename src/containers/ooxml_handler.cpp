// ooxml_handler.cpp
//
// Generic handler for OOXML containers (DOCX, XLSX, PPTX)
//

#include "ooxml_handler.hpp"
#include "../utils/logger.hpp"
#include "../utils/file_type.hpp"
#include "../utils/archive_formats.hpp"
#include "../containers/archive_handler.hpp"
#include <archive.h>
#include <archive_entry.h>
#include <vector>
#include <fstream>
#include <system_error>
#include <filesystem>
#include "../encoder/zopflipng_encoder.hpp"
#include "../utils/mime_detector.hpp"
#include "../utils/random_utils.hpp"

// helper: map mime to containerformat using provided tables
static ContainerFormat mime_to_container_format(const std::string &mime) {
    auto it = mime_to_format.find(mime);
    if (it != mime_to_format.end()) {
        return it->second;
    }
    return ContainerFormat::Unknown;
}

static const char* handler_tag_for(ContainerFormat fmt) {
    switch (fmt) {
        case ContainerFormat::Docx: return "OoxmlHandler(DOCX)";
        case ContainerFormat::Xlsx: return "OoxmlHandler(XLSX)";
        case ContainerFormat::Pptx: return "OoxmlHandler(PPTX)";
        default: return "OoxmlHandler";
    }
}

static const char* ext_for(ContainerFormat fmt) {
    switch (fmt) {
        case ContainerFormat::Docx: return ".docx";
        case ContainerFormat::Xlsx: return ".xlsx";
        case ContainerFormat::Pptx: return ".pptx";
        default: return ".zip";
    }
}

ContainerJob OoxmlHandler::prepare(const std::string &path) {
    const char* tag = handler_tag_for(fmt_);
    Logger::log(LogLevel::Info, "Preparing OOXML handler: " + path, tag);

    ContainerJob job;
    job.original_path = path;
    job.format = fmt_;

    std::string prefix =
        (fmt_ == ContainerFormat::Docx ? "docx_" :
         fmt_ == ContainerFormat::Xlsx ? "xlsx_" :
         fmt_ == ContainerFormat::Pptx ? "pptx_" : "ooxml_");
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path() / (prefix + RandomUtils::random_suffix());
    std::filesystem::create_directories(temp_dir);
    job.temp_dir = temp_dir.string();

    archive *in = archive_read_new();
    archive_read_support_format_zip(in);
    int open_r = archive_read_open_filename(in, path.c_str(), 10240);
    if (open_r != ARCHIVE_OK && open_r != ARCHIVE_WARN) {
        Logger::log(LogLevel::Error, "Failed to open OOXML for reading: " + std::string(archive_error_string(in)), tag);
        archive_read_free(in);
        return job;
    }
    if (open_r == ARCHIVE_WARN) {
        Logger::log(LogLevel::Warning, std::string("LIBARCHIVE WARN: ") + archive_error_string(in), tag);
    }

    archive_entry *entry;
    int r = ARCHIVE_OK;
    while ((r = archive_read_next_header(in, &entry)) == ARCHIVE_OK) {
        const char *ename = archive_entry_pathname(entry);
        if (!ename) {
            Logger::log(LogLevel::Warning, "Entry with null name skipped", tag);
            archive_read_data_skip(in);
            continue;
        }

        std::string name = ename;
        std::filesystem::path out_path = temp_dir / name;
        std::error_code ec;
        std::filesystem::create_directories(out_path.parent_path(), ec);
        if (ec) {
            Logger::log(LogLevel::Error,
                        "Failed to create parent dir: " + out_path.parent_path().string() + " (" + ec.message() + ")",
                        tag);
            archive_read_data_skip(in);
            continue;
        }

        std::ofstream ofs(out_path, std::ios::binary);
        if (!ofs) {
            Logger::log(LogLevel::Error, "Failed to create file during extraction: " + out_path.string(), tag);
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
                Logger::log(LogLevel::Error, "Error reading data block: " + std::string(archive_error_string(in)), tag);
                break;
            }
            ofs.write(reinterpret_cast<const char *>(buff), static_cast<std::streamsize>(size));
        }
        ofs.close();

        // decide if this entry is itself a container using mime detection
        const std::string mime = MimeDetector::detect(out_path.string());
        const ContainerFormat fmt = mime_to_container_format(mime);

        if (fmt != ContainerFormat::Unknown && can_read_format(fmt)) {
            Logger::log(LogLevel::Debug, "Found nested container in OOXML: " + out_path.string() + " (" + mime + ")", tag);
            if (fmt == this->fmt_) {
                OoxmlHandler nested(this->fmt_);
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
        Logger::log(LogLevel::Error, "Iteration error: " + std::string(archive_error_string(in)), tag);
    }

    archive_read_close(in);
    archive_read_free(in);

    Logger::log(LogLevel::Debug, std::string("OOXML prepare complete: ")
                                 + std::to_string(job.file_list.size()) +
                                 " files, " + std::to_string(job.children.size()) + " nested containers", tag);

    return job;
}

bool OoxmlHandler::finalize(const ContainerJob &job, Settings &settings) {
    const char* tag = handler_tag_for(fmt_);
    Logger::log(LogLevel::Info, "Finalizing OOXML container: " + job.original_path.filename().string(), tag);

    namespace fs = std::filesystem;
    std::error_code ec;

    // finalize children first
    for (const auto &child: job.children) {
        bool child_ok = false;
        if (child.format == this->fmt_) {
            OoxmlHandler nested(this->fmt_);
            child_ok = nested.finalize(child, settings);
        } else {
            ArchiveHandler ah;
            child_ok = ah.finalize(child, settings);
        }
        if (!child_ok) {
            Logger::log(LogLevel::Error, "Failed to finalize nested container: " + child.original_path.filename().string(), tag);
            return false;
        }
    }

    fs::path src_path(job.original_path);
    fs::path tmp_path = src_path.parent_path() / (src_path.stem().string() + "_tmp" + ext_for(this->fmt_));

    struct archive *out = archive_write_new();
    if (!out) {
        Logger::log(LogLevel::Error, "archive_write_new failed", tag);
        return false;
    }

    // set ZIP format and force deflate compression
    int set_fmt = archive_write_set_format_zip(out);
    if (set_fmt == ARCHIVE_WARN) {
        Logger::log(LogLevel::Warning, std::string("LIBARCHIVE WARN: ") + archive_error_string(out), tag);
    }
    if (set_fmt != ARCHIVE_OK) {
        Logger::log(LogLevel::Error, "Failed to set ZIP format: " + std::string(archive_error_string(out)), tag);
        archive_write_free(out);
        return false;
    }
    archive_write_set_options(out, "compression=deflate");

    int open_w = archive_write_open_filename(out, tmp_path.string().c_str());
    if (open_w == ARCHIVE_WARN) {
        Logger::log(LogLevel::Warning, std::string("LIBARCHIVE WARN: ") + archive_error_string(out), tag);
    }
    if (open_w != ARCHIVE_OK) {
        Logger::log(LogLevel::Error, "Failed to open temp OOXML for writing: " + std::string(archive_error_string(out)), tag);
        archive_write_free(out);
        return false;
    }

    // ensure [Content_Types].xml is written first
    std::vector<std::string> files_ordered;
    auto it = std::find_if(job.file_list.begin(), job.file_list.end(),
                           [](const std::string &f){ return fs::path(f).filename() == "[Content_Types].xml"; });
    if (it != job.file_list.end()) {
        files_ordered.push_back(*it);
    }
    for (const auto &f : job.file_list) {
        if (fs::path(f).filename() != "[Content_Types].xml") {
            files_ordered.push_back(f);
        }
    }

    // write all entries
    for (const auto &file: files_ordered) {
        fs::path rel = fs::relative(file, job.temp_dir, ec);
        if (ec) rel = fs::path(file).filename();

        std::ifstream ifs(file, std::ios::binary);
        if (!ifs) {
            Logger::log(LogLevel::Error, "Failed to open file for reading: " + file, tag);
            continue;
        }
        std::vector<unsigned char> buf((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());

        std::vector<unsigned char> final_data;
        const auto ext = rel.extension().string();

        // recompress only PNG/JPG images, leave XML and others untouched
        if (ext == ".png" || ext == ".jpg" || ext == ".jpeg") {
            final_data = ZopfliPngEncoder::recompress_with_zopfli(buf);
            Logger::log(LogLevel::Debug,
                        "Recompressed image: " + rel.string() + " (" +
                        std::to_string(buf.size()) + " -> " +
                        std::to_string(final_data.size()) + " bytes)", tag);
        } else {
            final_data = buf;
            Logger::log(LogLevel::Debug, "Copied entry unchanged: " + rel.string(), tag);
        }

        archive_entry *entry = archive_entry_new();
        if (!entry) {
            Logger::log(LogLevel::Error, "archive_entry_new failed", tag);
            archive_write_close(out);
            archive_write_free(out);
            return false;
        }

        archive_entry_set_pathname(entry, rel.generic_string().c_str());
        archive_entry_set_size(entry, static_cast<la_int64_t>(final_data.size()));
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0644);
        archive_entry_set_mtime(entry, 0, 0); // determinism

        int wh = archive_write_header(out, entry);
        if (wh == ARCHIVE_WARN) {
            Logger::log(LogLevel::Warning, std::string("LIBARCHIVE WARN: ") + archive_error_string(out), tag);
        }
        if (wh != ARCHIVE_OK) {
            Logger::log(LogLevel::Error,
                        "Failed to write header for: " + rel.string() +
                        " (" + std::string(archive_error_string(out)) + ")", tag);
            archive_entry_free(entry);
            archive_write_close(out);
            archive_write_free(out);
            return false;
        }

        la_ssize_t wrote = archive_write_data(out, final_data.data(), final_data.size());
        if (wrote < 0) {
            Logger::log(LogLevel::Error,
                        "Failed to write data for: " + rel.string() +
                        " (" + std::string(archive_error_string(out)) + ")", tag);
            archive_entry_free(entry);
            archive_write_close(out);
            archive_write_free(out);
            return false;
        }

        archive_entry_free(entry);
    }

    int close_w = archive_write_close(out);
    if (close_w != ARCHIVE_OK) {
        Logger::log(LogLevel::Error, "Failed to close archive: " + std::string(archive_error_string(out)), tag);
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
            Logger::log(LogLevel::Error, "Failed to replace original OOXML: " + ec.message(), tag);
            return false;
        }
        Logger::log(LogLevel::Info,
                    "Optimized OOXML: " + src_path.string() +
                    " (" + std::to_string(orig_size) + " -> " + std::to_string(new_size) + " bytes)", tag);
    } else {
        fs::remove(tmp_path, ec);
        Logger::log(LogLevel::Debug, "No improvement for: " + src_path.string(), tag);
    }

    // cleanup temp dir
    fs::remove_all(job.temp_dir, ec);
    if (ec) {
        Logger::log(LogLevel::Warning, "Can't remove temp dir: " + job.temp_dir.filename().string() + " (" + ec.message() + ")", tag);
    } else {
        Logger::log(LogLevel::Debug, "Removed temp dir: " + job.temp_dir.filename().string(), tag);
    }

    return true;
}