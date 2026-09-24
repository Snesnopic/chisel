//
// Created by Giuseppe Francione on 23/09/26.
//

/**
 * @file jpeg_structure.hpp
 * @brief Parsing of what follows a JPEG's first image: MPF images, trailers and XMP links.
 */

#ifndef CHISEL_JPEG_STRUCTURE_HPP
#define CHISEL_JPEG_STRUCTURE_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace chisel::jpeg {

/**
 * @brief Finds the end of the first image by walking its markers.
 * @return Offset just past the first EOI, or std::nullopt for a malformed stream.
 */
std::optional<std::size_t> first_image_end(std::span<const uint8_t> data);

/**
 * @brief Location of the MP Entry table in an image's APP2 "MPF" segment (CIPA DC-007).
 */
struct MpfIndex {
    std::size_t base = 0;    ///< MP Endian field; entry offsets are relative to it
    std::size_t entries = 0; ///< first 16-byte MP entry
    std::size_t count = 0;
    bool little_endian = false;
};

/**
 * @brief Finds the MP Entry table in the header of an image.
 * @param image The image (or the file starting with it).
 * @param limit Header search bound, usually the image's EOI.
 */
std::optional<MpfIndex> find_mpf_index(std::span<const uint8_t> image, std::size_t limit);

uint32_t read32(std::span<const uint8_t> d, std::size_t at, bool little_endian);
void write32(std::span<uint8_t> d, std::size_t at, uint32_t value, bool little_endian);

/**
 * @brief An MPF secondary image (gain map, MPO view, preview).
 */
struct SecondaryImage {
    std::size_t entry = 0; ///< MP entry index
    std::size_t start = 0;
    std::size_t size = 0;
};

/**
 * @brief Everything after the first image: MPF images, trailing data, then IPTC trailers.
 */
struct Layout {
    std::size_t primary_end = 0;
    std::optional<MpfIndex> mpf;
    std::vector<SecondaryImage> images; ///< sorted by start
    std::size_t trailing_start = 0;     ///< end of the last image, or of the primary
    std::size_t trailers_start = 0;     ///< first byte of the stacked IPTC trailers
};

/**
 * @brief Parses the structure after the first image.
 * @return std::nullopt if the first image is malformed or an MPF entry is dangling or overlapping.
 */
std::optional<Layout> parse_layout(std::span<const uint8_t> data);

/**
 * @brief Start of the AFCP, FotoStation and Photo Mechanic trailers stacked at the end of the file.
 * @param from Lowest offset the trailers may start at.
 */
std::size_t metadata_trailers_start(std::span<const uint8_t> data, std::size_t from);

/// @brief True for a non-empty run of only 0x00 or only 0xFF bytes.
bool is_padding(std::span<const uint8_t> bytes);

/**
 * @brief Fixes the absolute offsets of AFCP trailers in a block of stacked trailers that moved.
 * @param block The trailers as they will be written.
 * @param old_pos Where the block started in the original file.
 * @param new_pos Where it starts in the output.
 */
void relocate_trailers(std::span<uint8_t> block, std::size_t old_pos, std::size_t new_pos);

/// @brief Trailing data minus IPTC trailers and pure padding, for integrity comparisons.
std::span<const uint8_t> trailing_content(std::span<const uint8_t> data, const Layout& layout);

inline constexpr std::string_view kXmpId{"http://ns.adobe.com/xap/1.0/\0", 29};
inline constexpr std::string_view kExifId{"Exif\0\0", 6};

/// @brief Returns the standard XMP packet of an image's header, if any.
std::optional<std::string> find_xmp(std::span<const uint8_t> image, std::size_t limit);

/// @brief Returns the EXIF APP1 payload (starting with "Exif\0\0") of an image's header, if any.
std::optional<std::vector<uint8_t>> find_exif(std::span<const uint8_t> image, std::size_t limit);

/// @brief True if the image's XMP marks it as an Apple HDR gain map.
bool is_apple_gain_map(std::span<const uint8_t> image);

/**
 * @brief Builds an EXIF payload holding only the Apple maker note tags that drive HDR
 *        rendering (HDRHeadroom, HDRGain).
 * @param exif The original EXIF payload.
 * @return The new payload, or std::nullopt if the maker note carries no HDR data.
 */
std::optional<std::vector<uint8_t>> apple_hdr_exif(std::span<const uint8_t> exif);

/**
 * @brief Keeps only the properties that tie secondary content to the image
 *        (hdrgm, GContainer, GCamera motion photo, Apple HDR gain map).
 * @return The reduced packet, or an empty string if nothing is left.
 */
std::string reduce_xmp(std::string_view packet);

/**
 * @brief Sets Item:Length of the GContainer items with the given semantic.
 * @return The rewritten packet, or std::nullopt if there is no such item.
 */
std::optional<std::string> set_item_length(std::string_view packet, std::string_view semantic, uint64_t length);

/**
 * @brief GContainer item as declared in XMP.
 */
struct ContainerItem {
    std::string semantic;
    std::string mime;
    uint64_t length = 0;
};

/**
 * @brief Links from a primary image to media appended after it.
 */
struct XmpLinks {
    std::optional<uint64_t> micro_video_offset; ///< legacy motion photo: video size, counted from the end
    std::vector<ContainerItem> items;
};

XmpLinks parse_xmp_links(std::string_view packet);

} // namespace chisel::jpeg

#endif // CHISEL_JPEG_STRUCTURE_HPP
