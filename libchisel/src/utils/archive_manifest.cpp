//
// Created by Giuseppe Francione on 23/09/26.
//

#include "../../include/archive_manifest.hpp"
#include "../../include/logger.hpp"
#include "file_utils.hpp"
#include <archive.h>
#include <archive_entry.h>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <string>

namespace chisel {

namespace fs = std::filesystem;

namespace {

// the zip reader zero-fills fields zips may lack (uid/gid, atime/ctime), which would add extra fields
std::shared_ptr<archive_entry> zip_entry_copy(archive_entry* source) {
    std::shared_ptr<archive_entry> entry(archive_entry_new(), archive_entry_free);
    if (!entry) return entry;
    archive_entry_copy_pathname(entry.get(), archive_entry_pathname(source));
    archive_entry_set_mode(entry.get(), archive_entry_mode(source));
    archive_entry_set_mtime(entry.get(), archive_entry_mtime(source), archive_entry_mtime_nsec(source));
    if (archive_entry_atime(source) != 0) archive_entry_set_atime(entry.get(), archive_entry_atime(source), 0);
    if (archive_entry_ctime(source) != 0) archive_entry_set_ctime(entry.get(), archive_entry_ctime(source), 0);
    if (archive_entry_uid(source) != 0 || archive_entry_gid(source) != 0) {
        archive_entry_set_uid(entry.get(), archive_entry_uid(source));
        archive_entry_set_gid(entry.get(), archive_entry_gid(source));
    }
    if (const char* target = archive_entry_symlink(source)) archive_entry_copy_symlink(entry.get(), target);
    if (archive_entry_size_is_set(source)) archive_entry_set_size(entry.get(), archive_entry_size(source));
    return entry;
}

// the entry's own path under dest_dir, or a folder of its own when that path is unsafe or taken
fs::path extraction_path(archive_entry* entry, const fs::path& dest_dir, const std::size_t index) {
    const char* raw = archive_entry_pathname(entry);
    std::string name = raw != nullptr ? raw : "";
    for (auto& c : name) {
        if (c == '\\') c = '/';
    }

    std::error_code ec;
    fs::path out;
    if (sanitize_archive_entry_path(name, dest_dir, out) && out.has_filename() &&
        out != dest_dir.lexically_normal() && !fs::exists(fs::symlink_status(out, ec))) {
        fs::create_directories(out.parent_path(), ec);
        if (!ec && fs::is_directory(out.parent_path(), ec)) return out;
    }

    // keep the extension, processors may fall back on it
    const auto extension = fs::path(name).extension().string();
    const auto leaf = "entry" + (extension.size() <= 16 ? extension : std::string{});
    for (std::size_t n = index;; ++n) {
        const auto dir = dest_dir / (".chisel-" + std::to_string(n));
        if (fs::exists(fs::symlink_status(dir, ec))) continue;
        fs::create_directories(dir, ec);
        return ec ? fs::path{} : dir / leaf;
    }
}

bool extract_data(archive* a, archive_entry* entry, const fs::path& out, const std::string_view tag) {
    std::ofstream os(out, std::ios::binary);
    if (!os) {
        Logger::log(LogLevel::Error, "Can't create " + out.string(), tag);
        return false;
    }
    const void* block = nullptr;
    std::size_t size = 0;
    la_int64_t offset = 0;
    la_int64_t end = 0;
    int r = ARCHIVE_OK;
    while ((r = archive_read_data_block(a, &block, &size, &offset)) == ARCHIVE_OK) {
        // sparse entries skip their holes
        if (offset != end) os.seekp(offset);
        os.write(static_cast<const char*>(block), static_cast<std::streamsize>(size));
        end = offset + static_cast<la_int64_t>(size);
    }
    os.close();
    if (r != ARCHIVE_EOF || !os) {
        Logger::log(LogLevel::Error, "Can't extract " + out.filename().string() + ": " +
                    (archive_error_string(a) != nullptr ? archive_error_string(a) : "write error"), tag);
        return false;
    }
    if (archive_entry_size_is_set(entry) && end < archive_entry_size(entry)) {
        std::error_code ec;
        fs::resize_file(out, static_cast<std::uintmax_t>(archive_entry_size(entry)), ec);
        if (ec) return false;
    }
    return true;
}

} // namespace

std::optional<ArchiveManifest> read_archive_manifest(const fs::path& input, const fs::path& dest_dir,
                                                     const bool zip_only, const std::string_view tag) {
    const std::unique_ptr<archive, decltype(&archive_read_free)> a(archive_read_new(), archive_read_free);
    if (!a) return std::nullopt;
    if (zip_only) {
        archive_read_support_format_zip(a.get());
    } else {
        archive_read_support_filter_all(a.get());
        archive_read_support_format_all(a.get());
        archive_read_set_options(a.get(), "hdrcharset=UTF-8");
    }
#ifdef _WIN32
    int r = archive_read_open_filename_w(a.get(), input.wstring().c_str(), 10240);
#else
    int r = archive_read_open_filename(a.get(), input.string().c_str(), 10240);
#endif
    if (r == ARCHIVE_WARN) {
        Logger::log(LogLevel::Warning, std::string("LIBARCHIVE WARN: ") + archive_error_string(a.get()), tag);
    } else if (r != ARCHIVE_OK) {
        Logger::log(LogLevel::Error, "Can't open archive: " + std::string(archive_error_string(a.get())), tag);
        return std::nullopt;
    }

    ArchiveManifest manifest;
    archive_entry* entry = nullptr;
    while ((r = archive_read_next_header(a.get(), &entry)) != ARCHIVE_EOF) {
        // a warning can mean a name or owner that didn't convert, which couldn't be written back
        if (r != ARCHIVE_OK) {
            Logger::log(LogLevel::Error, "Can't read entry: " + std::string(archive_error_string(a.get())), tag);
            return std::nullopt;
        }

        const bool zip = (archive_format(a.get()) & ARCHIVE_FORMAT_BASE_MASK) == ARCHIVE_FORMAT_ZIP;
        ArchiveItem item;
        item.entry = zip ? zip_entry_copy(entry)
                         : std::shared_ptr<archive_entry>(archive_entry_clone(entry), archive_entry_free);
        if (!item.entry) return std::nullopt;
        manifest.format = std::max(manifest.format, archive_format(a.get()));
        if (const char* format_name = archive_format_name(a.get())) {
            item.stored = std::strstr(format_name, "(uncompressed)") != nullptr;
        }

        if (archive_entry_filetype(entry) == AE_IFREG && archive_entry_hardlink(entry) == nullptr) {
            item.data = extraction_path(entry, dest_dir, manifest.items.size());
            if (item.data.empty() || !extract_data(a.get(), entry, item.data, tag)) return std::nullopt;
        } else if (archive_read_data_skip(a.get()) != ARCHIVE_OK) {
            Logger::log(LogLevel::Error, "Can't skip entry: " + std::string(archive_error_string(a.get())), tag);
            return std::nullopt;
        }
        manifest.items.push_back(std::move(item));
    }
    return manifest;
}

std::vector<fs::path> manifest_files(const ArchiveManifest& manifest) {
    std::vector<fs::path> files;
    for (const auto& item : manifest.items) {
        if (!item.data.empty()) files.push_back(item.data);
    }
    return files;
}

bool write_archive_manifest(archive* a, const ArchiveManifest& manifest, const std::string_view tag) {
    const bool zip = (archive_format(a) & ARCHIVE_FORMAT_BASE_MASK) == ARCHIVE_FORMAT_ZIP;
    std::vector<char> buffer(64 * 1024);
    for (const auto& item : manifest.items) {
        const std::unique_ptr<archive_entry, decltype(&archive_entry_free)> entry(
            archive_entry_clone(item.entry.get()), archive_entry_free);
        if (!entry) return false;

        std::ifstream in;
        if (!item.data.empty()) {
            std::error_code ec;
            const auto size = fs::file_size(item.data, ec);
            in.open(item.data, std::ios::binary);
            if (ec || !in) {
                Logger::log(LogLevel::Error, "Can't read " + item.data.string(), tag);
                return false;
            }
            archive_entry_set_size(entry.get(), static_cast<la_int64_t>(size));
            archive_entry_sparse_clear(entry.get());
        }
        if (zip) {
            if (item.stored) {
                archive_write_zip_set_compression_store(a);
            } else {
                archive_write_zip_set_compression_deflate(a);
            }
        }

        const int r = archive_write_header(a, entry.get());
        if (r == ARCHIVE_WARN) {
            Logger::log(LogLevel::Warning, std::string("LIBARCHIVE WARN: ") + archive_error_string(a), tag);
        } else if (r != ARCHIVE_OK) {
            Logger::log(LogLevel::Error, "Can't write entry header: " + std::string(archive_error_string(a)), tag);
            return false;
        }
        while (in && in.is_open()) {
            in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            if (const auto got = in.gcount(); got > 0 &&
                archive_write_data(a, buffer.data(), static_cast<std::size_t>(got)) < 0) {
                Logger::log(LogLevel::Error, "Can't write entry data: " + std::string(archive_error_string(a)), tag);
                return false;
            }
        }
        if (archive_write_finish_entry(a) < ARCHIVE_WARN) {
            Logger::log(LogLevel::Error, "Can't finish entry: " + std::string(archive_error_string(a)), tag);
            return false;
        }
    }
    return true;
}

} // namespace chisel
