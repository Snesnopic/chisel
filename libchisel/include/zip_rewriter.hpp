//
// Created by Giuseppe Francione on 20/10/26.
//

/**
 * @file zip_rewriter.hpp
 * @brief Zip extraction and rewriting that keeps every header as it was stored.
 *
 * Only the entries' data, CRCs, sizes and offsets change on the way back: names as raw bytes,
 * flags, DOS times, extra fields, attributes, comments, prefixes and gaps are copied as they are.
 */
#ifndef CHISEL_ZIP_REWRITER_HPP
#define CHISEL_ZIP_REWRITER_HPP

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace chisel {

struct ProcessingOptions;

/**
 * @brief A zip entry as stored, and where its content was extracted.
 */
struct ZipEntry {
    std::vector<std::uint8_t> central; ///< central directory record, with its name, extra field and comment
    std::vector<std::uint8_t> local;   ///< local header, with its name and extra field
    std::uint64_t local_at = 0;        ///< absolute offset of the local header
    std::uint64_t data_at = 0;         ///< absolute offset of the compressed data
    std::uint64_t compressed = 0;      ///< compressed size
    std::uint64_t size = 0;            ///< uncompressed size
    std::uint32_t crc = 0;
    std::uint16_t method = 0;
    std::uint16_t flags = 0;
    std::uint8_t descriptor = 0;       ///< bytes of the data descriptor after the data, 0 without one
    bool descriptor_signature = false;
    bool zip64_local = false;          ///< the local header carries its sizes in a zip64 extra field
    std::filesystem::path data;        ///< extracted content, empty for an entry copied as it is

    [[nodiscard]] std::uint64_t end() const { return data_at + compressed + descriptor; }
};

/**
 * @brief Everything a zip is written back from.
 */
struct ZipManifest {
    std::filesystem::path source;
    std::vector<ZipEntry> entries;   ///< central directory order
    std::vector<std::size_t> order;  ///< entries in the order their data lies in the file
    std::uint64_t base = 0;          ///< what the stored offsets count from
    std::uint64_t cd_at = 0;         ///< absolute offset of the central directory
    std::uint64_t cd_size = 0;
    std::uint64_t zip64_end_at = 0;  ///< absolute offset of the zip64 end record, 0 without one
    std::uint64_t end_at = 0;        ///< absolute offset of the end of central directory record
    std::uint64_t archive_end = 0;   ///< end of the archive comment; anything after it is trailing data
    std::uint64_t file_size = 0;
};

/**
 * @brief How write_zip_manifest() may change an entry beyond its data.
 */
struct ZipWriteOptions {
    bool deflate_stored = false; ///< stored entries may be deflated, except a leading "mimetype"
    bool keep_trailing = true;   ///< copy what follows the end of central directory record
};

/**
 * @brief Reads a zip's layout and extracts every plain, readable entry into dest_dir.
 *
 * Directories, symlinks, encrypted entries, methods other than stored and deflate, and entries
 * whose data doesn't match their CRC are not extracted and go back as they are.
 * @return std::nullopt when the file isn't a zip this can rewrite exactly (e.g. split archives).
 */
std::optional<ZipManifest> read_zip_manifest(const std::filesystem::path& input, const std::filesystem::path& dest_dir,
                                             std::string_view tag);

/// @brief Paths of the extracted entries, for the executor to process.
std::vector<std::filesystem::path> zip_manifest_files(const ZipManifest& manifest);

/**
 * @brief Writes the zip back with the extracted entries' current content.
 *
 * Each such entry keeps whichever is smallest of its original bytes (when its content didn't change),
 * libdeflate and zopfli; zopfli runs only on content libdeflate could shrink, on the thread pool.
 * @return False when it can't be written; a partial output is removed.
 */
bool write_zip_manifest(const ZipManifest& manifest, const std::filesystem::path& output,
                        const ProcessingOptions& options, const ZipWriteOptions& write_options, std::string_view tag);

/**
 * @brief Reads a rewritten zip back and compares it with the manifest it was written from.
 * @return Empty when every header matches but for the rewritten fields, every entry copied as it was is
 * byte for byte the same, every rewritten one inflates to its file, and libarchive reads it all; otherwise
 * the first difference.
 */
std::string compare_zip_manifest(const std::filesystem::path& rebuilt, const ZipManifest& manifest);

/**
 * @brief Writes the zip back to output, then reads it back.
 * @throws std::runtime_error when either fails, with output removed.
 */
void rewrite_zip(const ZipManifest& manifest, const std::filesystem::path& output, const ProcessingOptions& options,
                 const ZipWriteOptions& write_options, std::string_view tag);

} // namespace chisel

#endif // CHISEL_ZIP_REWRITER_HPP
