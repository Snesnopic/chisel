//
// Created by Giuseppe Francione on 19/10/25.
//

#include "../../include/ooxml_processor.hpp"
#include "../../include/logger.hpp"
#include "../../include/random_utils.hpp"
#include "../../include/file_type.hpp"
#include <archive.h>
#include <archive_entry.h>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>
#include <algorithm>
#include "zlib_container.h"
#include "zopfli.h"

namespace chisel {

namespace fs = std::filesystem;

/**
 * @brief Recompresses a byte vector using Zopfli's Zlib implementation.
 * @param input The raw data to be compressed.
 * @return A vector containing the Zlib-compressed data.
 */
std::vector<unsigned char> recompress_with_zopfli(const std::vector<unsigned char>& input) {
        ZopfliOptions opts;
        ZopfliInitOptions(&opts);
        opts.numiterations = 15;
        opts.blocksplitting = 1;

        unsigned char* out_data = nullptr;
        size_t out_size = 0;
        ZopfliZlibCompress(&opts, input.data(), input.size(), &out_data, &out_size);

        std::vector<unsigned char> result(out_data, out_data + out_size);
        free(out_data);
        return result;
    }

/**
 * @brief Returns the tag used for logging by this processor.
 * @return A constant string identifier.
 */
static const char* processor_tag() {
    return "OOXMLProcessor";
}

/**
 * @brief Creates a unique temporary directory for a given input file.
 * @param input The path to the input file, used to create a descriptive directory name.
 * @param prefix A prefix for the temporary directory name.
 * @return The path to the newly created temporary directory.
 */
static fs::path make_temp_dir_for(const fs::path& input, const std::string& prefix) {
    const auto base_tmp = fs::temp_directory_path() / "chisel-ooxml";
    std::error_code ec;
    fs::create_directories(base_tmp, ec);

    const std::string stem = input.stem().string();
    const std::string dir_name = prefix + stem + "_" + RandomUtils::random_suffix();
    auto dir = base_tmp / dir_name;

    fs::create_directories(dir, ec);
    return dir;
}

/**
 * @brief Recursively removes a directory and its contents, logging any errors.
 * @param dir The path to the directory to be removed.
 */
static void cleanup_temp_dir(const fs::path& dir) {
    std::error_code ec;
    fs::remove_all(dir, ec);
    if (ec) {
        Logger::log(LogLevel::Warning, "Can't remove temp dir: " + dir.string() + " (" + ec.message() + ")", processor_tag());
    } else {
        Logger::log(LogLevel::Debug, "Removed temp dir: " + dir.string(), processor_tag());
    }
}

std::optional<ExtractedContent> OOXMLProcessor::prepare_extraction(const std::filesystem::path& input_path) {
    Logger::log(LogLevel::Info, "Preparing OOXML: " + input_path.filename().string(), processor_tag());

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

    archive* in = archive_read_new();
    archive_read_support_format_zip(in);
    int open_r = archive_read_open_filename(in, input_path.string().c_str(), 10240);
    if (open_r != ARCHIVE_OK && open_r != ARCHIVE_WARN) {
        Logger::log(LogLevel::Error, "Failed to open OOXML for reading: " + std::string(archive_error_string(in)), processor_tag());
        archive_read_free(in);
        cleanup_temp_dir(temp_dir);
        return content;
    }
    if (open_r == ARCHIVE_WARN) {
        Logger::log(LogLevel::Warning, std::string("LIBARCHIVE WARN: ") + archive_error_string(in), processor_tag());
    }

    archive_entry* entry = nullptr;
    int r = ARCHIVE_OK;
    while ((r = archive_read_next_header(in, &entry)) == ARCHIVE_OK) {
        const char* ename = archive_entry_pathname(entry);
        if (!ename) {
            Logger::log(LogLevel::Warning, "Entry with null name skipped", processor_tag());
            archive_read_data_skip(in);
            continue;
        }

        const std::string name = ename;
        const fs::path out_path = temp_dir / name;

        std::error_code ec;
        const auto filetype = archive_entry_filetype(entry);

        if (filetype == AE_IFDIR) {
            fs::create_directories(out_path, ec);
            if (ec) {
                Logger::log(LogLevel::Error, "Failed to create directory: " + out_path.string(), processor_tag());
            }
            archive_read_data_skip(in);
            continue;
        }

        fs::create_directories(out_path.parent_path(), ec);
        if (ec) {
            Logger::log(LogLevel::Error,
                        "Failed to create parent dir: " + out_path.parent_path().string() + " (" + ec.message() + ")",
                        processor_tag());
            archive_read_data_skip(in);
            continue;
        }

        std::ofstream ofs(out_path, std::ios::binary);
        if (!ofs) {
            Logger::log(LogLevel::Error, "Failed to create file during extraction: " + out_path.string(), processor_tag());
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
                Logger::log(LogLevel::Error, "Error reading data block: " + std::string(archive_error_string(in)), processor_tag());
                break;
            }
            ofs.write(reinterpret_cast<const char*>(buff), static_cast<std::streamsize>(size));
        }
        ofs.close();

        content.extracted_files.push_back(out_path);
    }

    if (r != ARCHIVE_EOF) {
        Logger::log(LogLevel::Error, "Iteration error: " + std::string(archive_error_string(in)), processor_tag());
    }

    archive_read_close(in);
    archive_read_free(in);

    Logger::log(LogLevel::Debug,
                "OOXML prepare complete: " + std::to_string(content.extracted_files.size()) + " files",
                processor_tag());

    return content;
}

std::filesystem::path OOXMLProcessor::finalize_extraction(const ExtractedContent& content,
                                                          ContainerFormat /*target_format*/) {
    Logger::log(LogLevel::Info, "Finalizing OOXML: " + content.original_path.filename().string(), processor_tag());

    namespace fs = std::filesystem;
    std::error_code ec;

    const fs::path src_path(content.original_path);
    const fs::path tmp_path = fs::temp_directory_path() /
                              (src_path.stem().string() + "_tmp" + RandomUtils::random_suffix() + src_path.extension().string());

    archive* out = archive_write_new();
    if (!out) {
        Logger::log(LogLevel::Error, "archive_write_new failed", processor_tag());
        cleanup_temp_dir(content.temp_dir);
        throw std::runtime_error("OOXMLProcessor: archive_write_new failed");
    }

    // set ZIP format and force deflate compression
    const int set_fmt = archive_write_set_format_zip(out);
    if (set_fmt == ARCHIVE_WARN) {
        Logger::log(LogLevel::Warning, std::string("LIBARCHIVE WARN: ") + archive_error_string(out), processor_tag());
    }
    if (set_fmt != ARCHIVE_OK) {
        Logger::log(LogLevel::Error, "Failed to set ZIP format: " + std::string(archive_error_string(out)), processor_tag());
        archive_write_free(out);
        cleanup_temp_dir(content.temp_dir);
        throw std::runtime_error("OOXMLProcessor: set_format_zip failed");
    }
    archive_write_set_options(out, "compression=deflate");

    const int open_w = archive_write_open_filename(out, tmp_path.string().c_str());
    if (open_w == ARCHIVE_WARN) {
        Logger::log(LogLevel::Warning, std::string("LIBARCHIVE WARN: ") + archive_error_string(out), processor_tag());
    }
    if (open_w != ARCHIVE_OK) {
        Logger::log(LogLevel::Error, "Failed to open temp OOXML for writing: " + std::string(archive_error_string(out)), processor_tag());
        archive_write_free(out);
        cleanup_temp_dir(content.temp_dir);
        throw std::runtime_error("OOXMLProcessor: open_filename failed");
    }

    // ensure [Content_Types].xml is written first
    std::vector<fs::path> files_ordered;
    auto it = std::find_if(content.extracted_files.begin(), content.extracted_files.end(),
                           [](const fs::path& f){ return f.filename() == "[Content_Types].xml"; });
    if (it != content.extracted_files.end()) {
        files_ordered.push_back(*it);
    }
    for (const auto& f : content.extracted_files) {
        if (fs::path(f).filename() != "[Content_Types].xml") {
            files_ordered.push_back(f);
        }
    }

    try {
        // write all entries (recompress images; copy others)
        for (const auto& file : files_ordered) {
            fs::path rel = fs::relative(file, content.temp_dir, ec);
            if (ec) rel = fs::path(file).filename();

            std::ifstream ifs(file, std::ios::binary);
            if (!ifs) {
                Logger::log(LogLevel::Error, "Failed to open file for reading: " + file.filename().string(), processor_tag());
                continue;
            }
            const std::vector<unsigned char> buf((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());

            std::vector<unsigned char> final_data;
            const auto ext = rel.extension().string();

            // recompress only PNG/JPG images, leave XML and others untouched
            if (ext == ".png" || ext == ".jpg" || ext == ".jpeg") {
                final_data = recompress_with_zopfli(buf);
                Logger::log(LogLevel::Debug,
                            "Recompressed image: " + rel.string() + " (" +
                            std::to_string(buf.size()) + " -> " +
                            std::to_string(final_data.size()) + " bytes)", processor_tag());
            } else {
                final_data = buf;
                Logger::log(LogLevel::Debug, "Copied entry unchanged: " + rel.string(), processor_tag());
            }

            archive_entry* entry = archive_entry_new();
            if (!entry) {
                Logger::log(LogLevel::Error, "archive_entry_new failed", processor_tag());
                throw std::runtime_error("OOXMLProcessor: archive_entry_new failed");
            }

            archive_entry_set_pathname(entry, rel.generic_string().c_str());
            archive_entry_set_size(entry, static_cast<la_int64_t>(final_data.size()));
            archive_entry_set_filetype(entry, AE_IFREG);
            archive_entry_set_perm(entry, 0644);
            archive_entry_set_mtime(entry, 0, 0); // determinism

            const int wh = archive_write_header(out, entry);
            if (wh == ARCHIVE_WARN) {
                Logger::log(LogLevel::Warning, std::string("LIBARCHIVE WARN: ") + archive_error_string(out), processor_tag());
            }
            if (wh != ARCHIVE_OK) {
                Logger::log(LogLevel::Error,
                            "Failed to write header for: " + rel.string() +
                            " (" + std::string(archive_error_string(out)) + ")", processor_tag());
                archive_entry_free(entry);
                throw std::runtime_error("OOXMLProcessor: write_header failed");
            }

            const la_ssize_t wrote = archive_write_data(out, final_data.data(), final_data.size());
            if (wrote < 0) {
                Logger::log(LogLevel::Error,
                            "Failed to write data for: " + rel.string() +
                            " (" + std::string(archive_error_string(out)) + ")", processor_tag());
                archive_entry_free(entry);
                throw std::runtime_error("OOXMLProcessor: write_data failed");
            }

            archive_entry_free(entry);
        }
    } catch (const std::exception& e) {
        // log the error before cleanup
        Logger::log(LogLevel::Error, "Failed to finalize OOXML: " + std::string(e.what()) + " for file: " + content.original_path.filename().string(), processor_tag());
        archive_write_close(out);
        archive_write_free(out);
        cleanup_temp_dir(content.temp_dir);
        throw;
    } catch (...) {
        // log unknown error
        Logger::log(LogLevel::Error, "Failed to finalize OOXML: Unknown exception for file: " + content.original_path.filename().string(), processor_tag());
        archive_write_close(out);
        archive_write_free(out);
        cleanup_temp_dir(content.temp_dir);
        throw;
    }

    const int close_w = archive_write_close(out);
    if (close_w != ARCHIVE_OK) {
        Logger::log(LogLevel::Error, "Failed to close archive: " + std::string(archive_error_string(out)), processor_tag());
        archive_write_free(out);
        cleanup_temp_dir(content.temp_dir);
        throw std::runtime_error("OOXMLProcessor: write_close failed");
    }
    archive_write_free(out);

    cleanup_temp_dir(content.temp_dir);

    return tmp_path;
}

std::string OOXMLProcessor::get_raw_checksum(const std::filesystem::path& /*file_path*/) const {
    // TODO: implement checksum of core data if needed (optional for OOXML)
    return "";
}

} // namespace chisel