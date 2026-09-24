//
// Created by Giuseppe Francione on 24/09/26.
//

/**
 * @file png_structure.hpp
 * @brief Parsing of a PNG's chunk layout: where the image ends and which chunks a re-encoding carries over.
 */

#ifndef CHISEL_PNG_STRUCTURE_HPP
#define CHISEL_PNG_STRUCTURE_HPP

#include <array>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace chisel::png {

/**
 * @brief Finds the end of the image by walking its chunks.
 * @return Offset just past the IEND chunk, or std::nullopt for a malformed stream.
 */
std::optional<std::size_t> image_end(std::span<const unsigned char> data);

/**
 * @brief Reads the data following the IEND chunk of a PNG file.
 * @return The bytes after the image, possibly none, or std::nullopt for an unreadable or malformed file.
 */
std::optional<std::vector<unsigned char>> trailing_data(const std::filesystem::path& file);

/**
 * @brief Ancillary chunks of a source image that a re-encoded image must carry, verbatim.
 */
struct CarriedChunks {
    /// before PLTE, between PLTE and IDAT, after IDAT
    std::array<std::vector<std::vector<unsigned char>>, 3> groups;
    /// a kept chunk (bKGD, sBIT, hIST, pCAL) is only valid with the source's color type and palette
    bool color_bound = false;
    /// an unknown chunk isn't safe to copy once the image data changes, so the image can't be re-encoded
    bool blocked = false;
};

/**
 * @brief Picks the ancillary chunks of an image that a re-encoding of its pixels keeps.
 *
 * tRNS is never carried, as encoders write their own; APNG chunks are carried only when
 * keep_animation is set, i.e. when the frames are copied rather than re-encoded. Without
 * preserve_metadata only the chunks that change how the image is shown or used are kept.
 * @param png The whole source image, trailing data excluded.
 * @return The chunks to carry, or std::nullopt for a malformed stream.
 */
std::optional<CarriedChunks> carried_chunks(const std::vector<unsigned char>& png, bool preserve_metadata,
                                            bool keep_animation);

/**
 * @brief Inserts carried chunks into a re-encoded image, each group at its own place.
 * @return false for a malformed image.
 */
bool insert_chunks(std::vector<unsigned char>& png, const CarriedChunks& chunks);

} // namespace chisel::png

#endif // CHISEL_PNG_STRUCTURE_HPP
