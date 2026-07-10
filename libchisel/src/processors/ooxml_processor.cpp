//
// Created by Giuseppe Francione on 19/10/25.
//

#include "../../include/ooxml_processor.hpp"
#include "../../include/logger.hpp"
#include "../../include/random_utils.hpp"
#include "../../include/file_type.hpp"
#include "../../include/zip_extract_util.hpp"
#include <archive.h>
#include <archive_entry.h>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>
#include <algorithm>
#include "file_utils.hpp"


namespace chisel {

namespace fs = std::filesystem;

std::optional<ExtractedContent> OOXMLProcessor::prepare_extraction(const std::filesystem::path& input_path) {
    Logger::log(LogLevel::Debug, "Entering prepare_extraction for " + input_path.filename().string(), get_name());

    ExtractedContent content;
    content.original_path = input_path;

    // choose a prefix based on extension for readability
    const auto ext = input_path.extension().string();
    const std::string prefix =
        (ext == ".docx" ? "docx_" :
         ext == ".xlsx" ? "xlsx_" :
         ext == ".pptx" ? "pptx_" : "ooxml_");

    content.format = parse_container_format(prefix.substr(0, prefix.size() - 1)).value_or(ContainerFormat::Unknown);

    const fs::path temp_dir = make_temp_dir_for(input_path, prefix);
    content.temp_dir = temp_dir;

    auto extracted = extract_zip_entries(input_path, temp_dir, get_name());
    if (!extracted) {
        cleanup_temp_dir(temp_dir, get_name());
        return std::nullopt;
    }
    content.extracted_files = std::move(*extracted);

    Logger::log(LogLevel::Debug, "Exiting prepare_extraction for " + input_path.filename().string(), get_name());

    return content;
}

std::filesystem::path OOXMLProcessor::finalize_extraction(const ExtractedContent& content, const ProcessingOptions &options) {
    Logger::log(LogLevel::Debug, "Entering finalize_extraction for " + content.original_path.filename().string(), get_name());

    namespace fs = std::filesystem;
    std::error_code ec;

    const fs::path src_path(content.original_path);
    const fs::path tmp_path = fs::temp_directory_path() /
                              (src_path.stem().string() + "_tmp" + RandomUtils::random_suffix() + src_path.extension().string());

    archive* out = archive_write_new();
    if (!out) {
        Logger::log(LogLevel::Error, "Archive_write_new failed", get_name());
        cleanup_temp_dir(content.temp_dir);
        throw std::runtime_error("OOXMLProcessor: archive_write_new failed");
    }

    // set ZIP format and force deflate compression
    const int set_fmt = archive_write_set_format_zip(out);
    if (set_fmt == ARCHIVE_WARN) {
        Logger::log(LogLevel::Warning, std::string("LIBARCHIVE WARN: ") + archive_error_string(out), get_name());
    }
    if (set_fmt != ARCHIVE_OK) {
        Logger::log(LogLevel::Error, "Failed to set zip format: " + std::string(archive_error_string(out)), get_name());
        archive_write_free(out);
        cleanup_temp_dir(content.temp_dir);
        throw std::runtime_error("OOXMLProcessor: set_format_zip failed");
    }
    archive_write_set_format_option(out, "zip", "compression", "deflate");
    archive_write_set_format_option(out, "zip", "compression-level", "9");

    const int open_w = archive_write_open_filename(out, tmp_path.string().c_str());
    if (open_w == ARCHIVE_WARN) {
        Logger::log(LogLevel::Warning, std::string("LIBARCHIVE WARN: ") + archive_error_string(out), get_name());
    }
    if (open_w != ARCHIVE_OK) {
        Logger::log(LogLevel::Error, "Failed to open temp ooxml for writing: " + std::string(archive_error_string(out)), get_name());
        archive_write_free(out);
        cleanup_temp_dir(content.temp_dir);
        throw std::runtime_error("OOXMLProcessor: open_filename failed");
    }

    // walk the temp dir directly (not content.extracted_files, which never includes
    // directories) so empty directory entries aren't silently dropped on rebuild
    std::vector<fs::path> all_entries;
    for (auto dit = fs::recursive_directory_iterator(content.temp_dir, ec);
         !ec && dit != fs::recursive_directory_iterator(); ++dit) {
        all_entries.push_back(dit->path());
    }

    // ensure [Content_Types].xml is written first
    std::vector<fs::path> files_ordered;
    auto it = std::find_if(all_entries.begin(), all_entries.end(),
                           [](const fs::path& f){ return f.filename() == "[Content_Types].xml"; });
    if (it != all_entries.end()) {
        files_ordered.push_back(*it);
    }
    for (const auto& f : all_entries) {
        if (f.filename() != "[Content_Types].xml") {
            files_ordered.push_back(f);
        }
    }

    try {
        // write all entries (recompress images; copy others)
        for (const auto& file : files_ordered) {
            fs::path rel = fs::relative(file, content.temp_dir, ec);
            if (ec) rel = fs::path(file).filename();

            const bool is_dir = fs::is_directory(file, ec);

            std::vector<unsigned char> final_data;
            if (!is_dir) {
                std::ifstream ifs(file, std::ios::binary);
                if (!ifs) {
                    Logger::log(LogLevel::Error, "Failed to open file for reading: " + file.filename().string(), get_name());
                    continue;
                }
                final_data.assign((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            }
            Logger::log(LogLevel::Debug, "Copied entry to OOXML: " + rel.string(), get_name());

            archive_entry* entry = archive_entry_new();
            if (!entry) {
                Logger::log(LogLevel::Error, "Archive_entry_new failed", get_name());
                throw std::runtime_error("OOXMLProcessor: archive_entry_new failed");
            }

            archive_entry_set_pathname(entry, rel.generic_string().c_str());
            archive_entry_set_size(entry, is_dir ? 0 : static_cast<la_int64_t>(final_data.size()));
            archive_entry_set_filetype(entry, is_dir ? AE_IFDIR : AE_IFREG);
            archive_entry_set_perm(entry, is_dir ? 0755 : 0644);
            archive_entry_set_mtime(entry, 0, 0); // determinism

            const int wh = archive_write_header(out, entry);
            if (wh == ARCHIVE_WARN) {
                Logger::log(LogLevel::Warning, std::string("LIBARCHIVE WARN: ") + archive_error_string(out), get_name());
            }
            if (wh != ARCHIVE_OK) {
                Logger::log(LogLevel::Error,
                            "Failed to write header for: " + rel.string() +
                            " (" + std::string(archive_error_string(out)) + ")", get_name());
                archive_entry_free(entry);
                throw std::runtime_error("OOXMLProcessor: write_header failed");
            }

            if (!is_dir) {
                const la_ssize_t wrote = archive_write_data(out, final_data.data(), final_data.size());
                if (wrote < 0) {
                    Logger::log(LogLevel::Error,
                                "Failed to write data for: " + rel.string() +
                                " (" + std::string(archive_error_string(out)) + ")", get_name());
                    archive_entry_free(entry);
                    throw std::runtime_error("OOXMLProcessor: write_data failed");
                }
            }

            archive_entry_free(entry);
        }
    } catch (const std::exception& e) {
        // log the error before cleanup
        Logger::log(LogLevel::Error, "Failed to finalize ooxml: " + std::string(e.what()) + " for file: " + content.original_path.filename().string(), get_name());
        archive_write_close(out);
        archive_write_free(out);
        cleanup_temp_dir(content.temp_dir);
        throw;
    } catch (...) {
        // log unknown error
        Logger::log(LogLevel::Error, "Failed to finalize ooxml: unknown exception for file: " + content.original_path.filename().string(), get_name());
        archive_write_close(out);
        archive_write_free(out);
        cleanup_temp_dir(content.temp_dir);
        throw;
    }

    const int close_w = archive_write_close(out);
    if (close_w != ARCHIVE_OK) {
        Logger::log(LogLevel::Error, "Failed to close archive: " + std::string(archive_error_string(out)), get_name());
        archive_write_free(out);
        cleanup_temp_dir(content.temp_dir);
        throw std::runtime_error("OOXMLProcessor: write_close failed");
    }
    Logger::log(LogLevel::Debug, "Exiting finalize_extraction for " + tmp_path.string(), get_name());
    archive_write_free(out);

    cleanup_temp_dir(content.temp_dir);

    return tmp_path;
}

std::string OOXMLProcessor::get_raw_checksum(const std::filesystem::path& /*file_path*/) const {
    // TODO: implement checksum of core data if needed (optional for OOXML)
    return "";
}

} // namespace chisel