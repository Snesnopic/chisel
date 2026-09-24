//
// Created by Giuseppe Francione on 24/09/26.
//

/**
 * @file data_uri.hpp
 * @brief Extraction and reinsertion of the base64 data URIs embedded in text files.
 */

#ifndef CHISEL_DATA_URI_HPP
#define CHISEL_DATA_URI_HPP

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace chisel {

/**
 * @brief A distinct data URI payload extracted to a file.
 */
struct DataUriAsset {
    std::size_t payload_hash = 0;    ///< hash of the base64 text, whitespace removed
    std::size_t payload_length = 0;  ///< length of the base64 text, whitespace removed
    std::size_t decoded_size = 0;    ///< size of the payload as extracted
    std::filesystem::path file;
};

/**
 * @brief Writes every distinct image or font payload of the base64 data URIs in text to its own file.
 *
 * A data URI is only taken when it is quoted (whitespace is then allowed in the payload) or
 * the whole argument of a CSS url(), and its payload is canonical base64.
 * @param make_dir Called once, before the first file is written, to get the folder for the files.
 */
std::vector<DataUriAsset> extract_data_uris(std::string_view text,
                                            const std::function<std::filesystem::path()>& make_dir);

/**
 * @brief Replaces the payloads whose extracted file got smaller with the file's content.
 * @return The rebuilt text, or std::nullopt if no payload changed.
 */
std::optional<std::string> reinsert_data_uris(std::string_view text, const std::vector<DataUriAsset>& assets);

} // namespace chisel

#endif // CHISEL_DATA_URI_HPP
