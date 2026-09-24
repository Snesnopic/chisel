//
// Created by Giuseppe Francione on 23/09/26.
//

/**
 * @file archive_manifest.hpp
 * @brief Archive extraction that remembers every entry, so the archive can be rebuilt as it was.
 */

#ifndef CHISEL_ARCHIVE_MANIFEST_HPP
#define CHISEL_ARCHIVE_MANIFEST_HPP

#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

struct archive;
struct archive_entry;

namespace chisel {

/**
 * @brief An archive entry as read, and where its data was extracted.
 */
struct ArchiveItem {
    std::shared_ptr<archive_entry> entry; ///< original header: name, type, times, mode, owner, links
    std::filesystem::path data;           ///< extracted content of a regular entry, empty for entries without data
    bool stored = false;                  ///< zip entry written without compression
};

/**
 * @brief Every entry of an archive, in archive order.
 */
struct ArchiveManifest {
    std::vector<ArchiveItem> items;
    int format = 0; ///< libarchive format code of the source, e.g. ARCHIVE_FORMAT_AR_GNU
};

/**
 * @brief Extracts the regular entries of an archive into dest_dir, keeping a manifest of all entries.
 *
 * Symlinks, hardlinks, directories and special files are only recorded, never created on disk.
 * Entries whose path is unsafe or collides with an earlier one (e.g. differing only by case on
 * a case-insensitive filesystem) are extracted to a folder of their own.
 * @param zip_only Read the input as a zip only.
 * @return The manifest, or std::nullopt if the archive can't be read completely.
 */
std::optional<ArchiveManifest> read_archive_manifest(const std::filesystem::path& input,
                                                     const std::filesystem::path& dest_dir,
                                                     bool zip_only,
                                                     std::string_view tag);

/// @brief Paths of the extracted entries, for the executor to process.
std::vector<std::filesystem::path> manifest_files(const ArchiveManifest& manifest);

/**
 * @brief Whether a zip carries a digital signature that repacking would invalidate.
 *
 * Detects signed JAR/APK/XPI entries, the APK Signature Scheme v2+ block, and the signature
 * parts of APPX/MSIX, NuGet, OPC (OOXML, XPS, VSIX), ODF, EPUB and iOS app packages.
 */
bool archive_is_signed(const std::filesystem::path& input);

/**
 * @brief Writes the manifest's entries, in order, with their original headers and current data.
 * @param a A writer already set to the output format and opened.
 */
bool write_archive_manifest(archive* a, const ArchiveManifest& manifest, std::string_view tag);

} // namespace chisel

#endif // CHISEL_ARCHIVE_MANIFEST_HPP
