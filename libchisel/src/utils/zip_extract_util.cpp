//
// Created by Giuseppe Francione on 08/07/26.
//

#include "../../include/zip_extract_util.hpp"
#include "../../include/logger.hpp"
#include "file_utils.hpp"
#include <archive.h>
#include <archive_entry.h>
#include <fstream>

namespace chisel {

namespace fs = std::filesystem;

std::optional<std::vector<fs::path>> extract_zip_entries(
    const fs::path& input_path,
    const fs::path& dest_dir,
    const std::string_view tag) {

    archive* in = archive_read_new();
    archive_read_support_format_zip(in);
    const int open_r = archive_read_open_filename(in, input_path.string().c_str(), 10240);
    if (open_r != ARCHIVE_OK && open_r != ARCHIVE_WARN) {
        Logger::log(LogLevel::Error, "Failed to open zip for reading: " + std::string(archive_error_string(in)), tag);
        archive_read_free(in);
        return std::nullopt;
    }
    if (open_r == ARCHIVE_WARN) {
        Logger::log(LogLevel::Warning, std::string("LIBARCHIVE WARN: ") + archive_error_string(in), tag);
    }

    std::vector<fs::path> extracted_files;
    archive_entry* entry = nullptr;
    int r = ARCHIVE_OK;
    while ((r = archive_read_next_header(in, &entry)) == ARCHIVE_OK) {
        const char* ename = archive_entry_pathname(entry);
        if (!ename) {
            Logger::log(LogLevel::Warning, "Entry with null name skipped", tag);
            archive_read_data_skip(in);
            continue;
        }

        fs::path out_path;
        if (!sanitize_archive_entry_path(ename, dest_dir, out_path)) {
            Logger::log(LogLevel::Warning, "Skipping suspicious archive entry (path traversal): " + std::string(ename), tag);
            archive_read_data_skip(in);
            continue;
        }

        const auto filetype = archive_entry_filetype(entry);
        std::error_code ec;

        if (filetype == AE_IFDIR) {
            fs::create_directories(out_path, ec);
            if (ec) {
                Logger::log(LogLevel::Error, "Failed to create directory: " + out_path.string(), tag);
            }
            archive_read_data_skip(in);
            continue;
        }

        if (filetype != AE_IFREG) {
            // symlinks, hardlinks, devices, etc. have no legitimate use in
            // these formats; skip rather than risk mishandling them.
            Logger::log(LogLevel::Warning, "Skipping non-regular entry: " + std::string(ename), tag);
            archive_read_data_skip(in);
            continue;
        }

        if (!ensure_parent_dirs(out_path, ec)) {
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

        const void* buff = nullptr;
        size_t size = 0;
        la_int64_t offset = 0;
        while (true) {
            const int rb = archive_read_data_block(in, &buff, &size, &offset);
            if (rb == ARCHIVE_EOF) break;
            if (rb != ARCHIVE_OK) {
                Logger::log(LogLevel::Error, "Error reading data block: " + std::string(archive_error_string(in)), tag);
                break;
            }
            ofs.write(static_cast<const char*>(buff), static_cast<std::streamsize>(size));
        }
        ofs.close();

        extracted_files.push_back(out_path);
    }

    if (r != ARCHIVE_EOF) {
        Logger::log(LogLevel::Error, "Iteration error: " + std::string(archive_error_string(in)), tag);
    }

    archive_read_close(in);
    archive_read_free(in);

    return extracted_files;
}

} // namespace chisel
