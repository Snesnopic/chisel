//
// Created by Giuseppe Francione on 20/10/25.
//

#include "../../include/archive_processor.hpp"
#include "../../include/archive_manifest.hpp"
#include "../../include/logger.hpp"
#include "../../include/mime_detector.hpp"
#include "../../include/file_type.hpp"
#include "../../include/random_utils.hpp"
#include <archive.h>
#include <filesystem>
#include <memory>
#include <string>
#include <optional>
#include <system_error>
#include "file_utils.hpp"

namespace chisel {

namespace fs = std::filesystem;


// --- format detection ---

/**
 * @brief Detects the container format of a file.
 * @param path The path to the file.
 * @return The detected ContainerFormat, or Unknown if not identified.
 */
static ContainerFormat detect_format(const fs::path& path) {
    const std::string mime = MimeDetector::detect(path);
    if (!mime.empty()) {
        const auto it = mime_to_format.find(mime);
        if (it != mime_to_format.end()) return it->second;
    }

    std::string ext = to_lower_copy(path.extension().string());
    if (!ext.empty() && ext.front() == '.') ext.erase(0, 1);
    if (!ext.empty()) {
        if (const auto parsed = parse_container_format(ext)) {
            return *parsed;
        }
    }
    return ContainerFormat::Unknown;
}

// --- libarchive create ---

// same tar variant as the source, so e.g. pax subsecond times survive
static int set_tar_format(archive* a, const int source_format) {
    if (source_format == ARCHIVE_FORMAT_TAR_USTAR || source_format == ARCHIVE_FORMAT_TAR_PAX_INTERCHANGE ||
        source_format == ARCHIVE_FORMAT_TAR_GNUTAR) {
        return archive_write_set_format(a, source_format);
    }
    return archive_write_set_format_pax_restricted(a);
}

static int set_cpio_format(archive* a, const int source_format) {
    switch (source_format) {
        case ARCHIVE_FORMAT_CPIO_POSIX:
        case ARCHIVE_FORMAT_CPIO_BIN_LE:
        case ARCHIVE_FORMAT_CPIO_SVR4_NOCRC:
        case ARCHIVE_FORMAT_CPIO_PWB:
            return archive_write_set_format(a, source_format);
        default:
            return archive_write_set_format_cpio(a);
    }
}

/**
 * @brief Rebuilds an archive from its manifest using libarchive.
 * @param manifest The entries read by prepare_extraction, with their current data.
 * @param out_path The path to the output archive file.
 * @param fmt The target container format for the new archive.
 * @return True on successful creation, false otherwise.
 */
static bool create_with_libarchive(const ArchiveManifest& manifest, const fs::path& out_path, ContainerFormat fmt) {
    const std::unique_ptr<archive, decltype(&archive_write_free)> owner(archive_write_new(), archive_write_free);
    archive* a = owner.get();
    if (!a) return false;

    int r = ARCHIVE_OK;

    switch (fmt) {
        case ContainerFormat::Epub:
        case ContainerFormat::Zip:
        case ContainerFormat::Cbz:
        case ContainerFormat::Jar:
        case ContainerFormat::Xpi:
        case ContainerFormat::Ora:
        case ContainerFormat::Dwfx:
        case ContainerFormat::Xps:
        case ContainerFormat::Apk:
            // entries stored in the source stay stored, see write_archive_manifest
            r = archive_write_set_format_zip(a);
            if (r == ARCHIVE_OK) {
                archive_write_set_format_option(a, "zip", "compression", "deflate");
                archive_write_set_format_option(a, "zip", "compression-level", "9");
            }
            break;
        case ContainerFormat::Tar:
        case ContainerFormat::Cbt:
            r = set_tar_format(a, manifest.format);
            break;
        case ContainerFormat::GZip:
            r = set_tar_format(a, manifest.format);
            if (r == ARCHIVE_OK) {
                r = archive_write_add_filter_gzip(a);
                if (r == ARCHIVE_OK) {
                    archive_write_set_filter_option(a, "gzip", "compression-level", "9");
                }
            }
            break;
        case ContainerFormat::BZip2:
            r = set_tar_format(a, manifest.format);
            if (r == ARCHIVE_OK) {
                r = archive_write_add_filter_bzip2(a);
                if (r == ARCHIVE_OK) {
                    archive_write_set_filter_option(a, "bzip2", "compression-level", "9");
                }
            }
            break;
        case ContainerFormat::Xz:
            r = set_tar_format(a, manifest.format);
            if (r == ARCHIVE_OK) {
                r = archive_write_add_filter_xz(a);
                if (r == ARCHIVE_OK) {
                    archive_write_set_filter_option(a, "xz", "compression-level", "9");
                }
            }
            break;
        case ContainerFormat::Zstd:
            r = set_tar_format(a, manifest.format);
            if (r == ARCHIVE_OK) {
                r = archive_write_add_filter_zstd(a);
                if (r == ARCHIVE_OK) {
                    archive_write_set_filter_option(a, "zstd", "compression-level", "22");
                }
            }
            break;

        case ContainerFormat::SevenZip:
            r = archive_write_set_format_7zip(a);
            if (r == ARCHIVE_OK) {
                archive_write_set_format_option(a, "7zip", "compression", "LZMA2");
                archive_write_set_format_option(a, "7zip", "compression-level", "9");
            }
            break;

        case ContainerFormat::Iso:
            r = archive_write_set_format_iso9660(a);
            if (r == ARCHIVE_OK) {
                archive_write_set_format_option(a, "iso9660", "joliet", "1");
                archive_write_set_format_option(a, "iso9660", "rockridge", "1");
                archive_write_set_format_option(a, "iso9660", "pad", "0");
            }
            break;

        case ContainerFormat::Cpio:
            r = set_cpio_format(a, manifest.format);
            break;

        case ContainerFormat::Ar:
            // GNU/SysV ar (e.g. .deb, GNU static libraries) keeps its own variant
            r = manifest.format == ARCHIVE_FORMAT_AR_GNU ? archive_write_set_format_ar_svr4(a)
                                                         : archive_write_set_format_ar_bsd(a);
            break;
        default:
            Logger::log(LogLevel::Error, "Unsupported output format for writing: " + container_format_to_string(fmt), "ArchiveProcessor");
            return false;
    }
    if (r == ARCHIVE_WARN) {
        Logger::log(LogLevel::Warning, std::string("LIBARCHIVE WARN: ") + archive_error_string(a), "ArchiveProcessor");
    }
    if (r != ARCHIVE_OK && r != ARCHIVE_WARN) {
        Logger::log(LogLevel::Error, "Setting format/filter failed: " + std::string(archive_error_string(a)), "ArchiveProcessor");
        return false;
    }

    #ifdef _WIN32
        r = archive_write_open_filename_w(a, out_path.wstring().c_str());
    #else
        r = archive_write_open_filename(a, out_path.string().c_str());
    #endif
    if (r == ARCHIVE_WARN) {
        Logger::log(LogLevel::Warning, std::string("LIBARCHIVE WARN: ") + archive_error_string(a), "ArchiveProcessor");
    }
    if (r != ARCHIVE_OK && r != ARCHIVE_WARN) {
        Logger::log(LogLevel::Error, "Archive_write_open_filename: " + std::string(archive_error_string(a)), "ArchiveProcessor");
        return false;
    }

    const bool written = write_archive_manifest(a, manifest, "ArchiveProcessor");
    if (archive_write_close(a) != ARCHIVE_OK) {
        Logger::log(LogLevel::Error, "Archive_write_close: " + std::string(archive_error_string(a)), "ArchiveProcessor");
        return false;
    }
    return written;
}

// --- IProcessor implementation ---

std::optional<ExtractedContent> ArchiveProcessor::prepare_extraction(const std::filesystem::path& input_path) {
    Logger::log(LogLevel::Debug, "Entering prepare_extraction for " + input_path.string(), get_name());

    ExtractedContent content;
    content.original_path = input_path;
    content.temp_dir = chisel::make_temp_dir_for(input_path, "archive");

    // Detect format
    content.format = detect_format(input_path);

    if (!can_read_format(content.format)) {
        Logger::log(LogLevel::Warning, "Unreadable or unrecognized format: " + input_path.filename().string(), get_name());
        cleanup_temp_dir(content.temp_dir, get_name());
        return std::nullopt;
    }

    auto manifest = read_archive_manifest(input_path, content.temp_dir, false, get_name());
    if (!manifest) {
        Logger::log(LogLevel::Error, "Extraction failed for: " + input_path.filename().string(), get_name());
        cleanup_temp_dir(content.temp_dir, get_name());
        return std::nullopt;
    }

    // nested archives need no special case, the executor re-detects every extracted file
    content.extracted_files = manifest_files(*manifest);
    content.extras = std::move(*manifest);

    Logger::log(
        LogLevel::Info,
        "Extracted files: " + std::to_string(content.extracted_files.size()),
        get_name()
    );

    Logger::log(LogLevel::Debug, "Exiting prepare_extraction for " + input_path.string(), get_name());
    return content;
}

std::filesystem::path ArchiveProcessor::finalize_extraction(const ExtractedContent& content, const ProcessingOptions &options) {
    Logger::log(LogLevel::Debug, "Entering finalize_extraction for " + content.original_path.string(), get_name());

    const auto out_fmt = content.format;
    const fs::path src_path(content.original_path);
    const std::string out_ext = "." + container_format_to_string(out_fmt);

    const fs::path tmp_archive = fs::temp_directory_path() /
                                 (src_path.stem().string() + "_tmp" + RandomUtils::random_suffix() + out_ext);

    const auto* manifest = std::any_cast<ArchiveManifest>(&content.extras);
    if (manifest == nullptr || !create_with_libarchive(*manifest, tmp_archive, out_fmt)) {
        Logger::log(LogLevel::Error, "Archive creation failed: " + tmp_archive.string(), get_name());
        fs::remove_all(content.temp_dir);
        // create_with_libarchive may have already opened/partially written tmp_archive
        // before failing mid-stream; don't leak the truncated file.
        std::error_code rm_ec;
        fs::remove(tmp_archive, rm_ec);
        throw std::runtime_error("ArchiveProcessor: create_with_libarchive failed");
    }

    std::error_code ec;
    if (!fs::exists(tmp_archive, ec) || ec) {
        Logger::log(LogLevel::Error, "Compressed archive not found: " + tmp_archive.string(), get_name());
        fs::remove_all(content.temp_dir);
        throw std::runtime_error("ArchiveProcessor: tmp archive missing");
    }

    chisel::cleanup_temp_dir(content.temp_dir, get_name());

    Logger::log(LogLevel::Debug, "Exiting finalize_extraction for " + tmp_archive.string(), get_name());
    return tmp_archive;
}

std::string ArchiveProcessor::get_raw_checksum(const std::filesystem::path& /*file_path*/) const {
    // TODO: define a meaningful checksum for archives if needed (optional)
    return "";
}

bool ArchiveProcessor::is_signed(const std::filesystem::path& file_path) const {
    return archive_is_signed(file_path);
}

} // namespace chisel