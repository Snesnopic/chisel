//
// Created by Giuseppe Francione on 08/07/26.
//

/**
 * @file zip_extract_util.hpp
 * @brief Shared, sanitized zip extraction helper for simple zip-based container formats.
 */

#ifndef CHISEL_ZIP_EXTRACT_UTIL_HPP
#define CHISEL_ZIP_EXTRACT_UTIL_HPP

#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

namespace chisel {

/**
 * @brief Extracts every regular-file entry of a zip archive into dest_dir,
 * preserving its internal directory structure.
 *
 * Used by processors (OdfProcessor, OOXMLProcessor) whose container format
 * is always a plain zip and only needs file contents, not permission,
 * symlink, or hardlink fidelity. Entry paths are sanitized against path
 * traversal (zip-slip); unsafe entries are skipped with a warning. Symlink
 * and other non-regular entries are skipped outright, since these formats
 * have no legitimate use for them.
 *
 * @param input_path Path to the zip file to read.
 * @param dest_dir Destination directory to extract into (must already exist).
 * @param tag Logger tag for diagnostic messages (typically the calling processor's get_name()).
 * @return Vector of extracted file paths, or std::nullopt if the archive could not be opened/read.
 */
std::optional<std::vector<std::filesystem::path>> extract_zip_entries(
    const std::filesystem::path& input_path,
    const std::filesystem::path& dest_dir,
    std::string_view tag);

} // namespace chisel

#endif // CHISEL_ZIP_EXTRACT_UTIL_HPP
