//
// Created by Giuseppe Francione on 19/10/25.
//

#include "../../include/odf_processor.hpp"
#include "../../include/logger.hpp"
#include "../../include/random_utils.hpp"
#include "../../include/file_type.hpp"
#include "../../include/archive_manifest.hpp"
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

std::optional<ExtractedContent> OdfProcessor::prepare_extraction(const std::filesystem::path& input_path) {
    Logger::log(LogLevel::Debug, "Entering prepare_extraction for " + input_path.filename().string(), get_name());

    ExtractedContent content;
    content.original_path = input_path;

    const auto ext = input_path.extension().string();
    const std::string prefix =
        (ext == ".odt" ? "odt_" :
         ext == ".ods" ? "ods_" :
         ext == ".odp" ? "odp_" :
         ext == ".odg" ? "odg_" : "odf_");
    content.format = parse_container_format(prefix.substr(0, prefix.size() - 1)).value_or(ContainerFormat::Unknown);

    const fs::path temp_dir = make_temp_dir_for(input_path, prefix);
    content.temp_dir = temp_dir;

    auto manifest = read_archive_manifest(input_path, temp_dir, true, get_name());
    if (!manifest) {
        cleanup_temp_dir(temp_dir, get_name());
        return std::nullopt;
    }
    content.extracted_files = manifest_files(*manifest);
    content.extras = std::move(*manifest);

    Logger::log(LogLevel::Debug, "Exiting prepare_extraction for " + input_path.string(), get_name());

    return content;
}

std::filesystem::path OdfProcessor::finalize_extraction(const ExtractedContent& content, const ProcessingOptions &options) {
    Logger::log(LogLevel::Debug, "Entering finalize_extraction for " + content.original_path.filename().string(), get_name());

    namespace fs = std::filesystem;

    const fs::path src_path(content.original_path);
    const fs::path tmp_path = fs::temp_directory_path() /
                              (src_path.stem().string() + "_tmp" + RandomUtils::random_suffix() + src_path.extension().string());

    archive* out = archive_write_new();
    if (out == nullptr) {
        Logger::log(LogLevel::Error, "Archive_write_new failed", get_name());
        cleanup_temp_dir(content.temp_dir);
        throw std::runtime_error("ODFProcessor: archive_write_new failed");
    }

    int set_fmt = archive_write_set_format_zip(out);
    if (set_fmt == ARCHIVE_WARN) {
        Logger::log(LogLevel::Warning, std::string("LIBARCHIVE WARN: ") + archive_error_string(out), get_name());
    }
    if (set_fmt != ARCHIVE_OK) {
        Logger::log(LogLevel::Error, "Failed to set zip format: " + std::string(archive_error_string(out)), get_name());
        archive_write_free(out);
        cleanup_temp_dir(content.temp_dir);
        throw std::runtime_error("ODFProcessor: set_format_zip failed");
    }
    archive_write_set_format_option(out, "zip", "compression", "deflate");
    archive_write_set_format_option(out, "zip", "compression-level", "9");

    int open_w = archive_write_open_filename(out, tmp_path.string().c_str());
    if (open_w == ARCHIVE_WARN) {
        Logger::log(LogLevel::Warning, std::string("LIBARCHIVE WARN: ") + archive_error_string(out), get_name());
    }
    if (open_w != ARCHIVE_OK) {
        Logger::log(LogLevel::Error, "Failed to open temp odf for writing: " + std::string(archive_error_string(out)), get_name());
        archive_write_free(out);
        cleanup_temp_dir(content.temp_dir);
        throw std::runtime_error("ODFProcessor: open_filename failed");
    }

    const auto* manifest = std::any_cast<ArchiveManifest>(&content.extras);
    if (manifest == nullptr || !write_archive_manifest(out, *manifest, get_name())) {
        Logger::log(LogLevel::Error, "Failed to finalize odf for file: " + content.original_path.filename().string(), get_name());
        archive_write_close(out);
        archive_write_free(out);
        cleanup_temp_dir(content.temp_dir);
        std::error_code rm_ec;
        fs::remove(tmp_path, rm_ec);
        throw std::runtime_error("ODFProcessor: writing entries failed");
    }

    int close_w = archive_write_close(out);
    if (close_w != ARCHIVE_OK) {
        Logger::log(LogLevel::Error, "Failed to close archive: " + std::string(archive_error_string(out)), get_name());
        archive_write_free(out);
        cleanup_temp_dir(content.temp_dir);
        throw std::runtime_error("ODFProcessor: write_close failed");
    }
    Logger::log(LogLevel::Debug, "Exiting finalize_extraction for " + tmp_path.string(), get_name());
    archive_write_free(out);

    cleanup_temp_dir(content.temp_dir);

    return tmp_path;
}

std::string OdfProcessor::get_raw_checksum(const std::filesystem::path& /*file_path*/) const {
    // TODO: implement checksum of core data if needed (optional for ODF)
    return "";
}

bool OdfProcessor::is_signed(const std::filesystem::path& file_path) const {
    return archive_is_signed(file_path);
}

} // namespace chisel