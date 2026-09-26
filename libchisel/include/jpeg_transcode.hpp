//
// Created by Giuseppe Francione on 26/09/26.
//

/**
 * @file jpeg_transcode.hpp
 * @brief Lossless re-encoding of one JPEG image held in memory, shared by the processors that carry JPEG data.
 */

#ifndef CHISEL_JPEG_TRANSCODE_HPP
#define CHISEL_JPEG_TRANSCODE_HPP

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace chisel::jpeg {

/**
 * @brief Which markers of an image survive re-encoding.
 */
struct MarkerPolicy {
    bool keep_all = true;                    ///< preserve metadata
    bool keep_links = false;                 ///< without metadata, still keep MPF and ISO gain map segments
    std::optional<std::string> xmp;          ///< replacement standard XMP packet; empty drops it
    std::optional<std::vector<uint8_t>> exif; ///< replacement EXIF payload
};

/**
 * @brief Which scan layouts a re-encoding may choose from.
 */
enum class ScanMode {
    Smallest, ///< progressive or sequential, whichever is smaller
    AsSource  ///< progressive only if the source was, for containers that declare it (JNG)
};

/**
 * @brief Losslessly re-encodes one JPEG image held in memory. Under ScanMode::Smallest it keeps the smaller of a
 *        progressive and a baseline encoding: on small images the headers of the progressive scans outweigh their gain.
 * @param clean Set when libjpeg ended the image exactly at the end of @p in, without warnings.
 * @return False on a libjpeg error.
 */
bool transcode(std::span<const uint8_t> in, const MarkerPolicy& policy, ScanMode mode, std::vector<uint8_t>& out,
               bool& clean);

/**
 * @brief Decoded samples of an image.
 */
struct Pixels {
    int width = 0;
    int height = 0;
    int channels = 0;
    std::vector<unsigned char> data;
    bool operator==(const Pixels&) const = default;
};

/**
 * @brief Decodes the first image of a JPEG held in memory.
 * @return False if it can't be decoded.
 */
bool decode_pixels(std::span<const uint8_t> in, Pixels& px);

} // namespace chisel::jpeg

#endif // CHISEL_JPEG_TRANSCODE_HPP
