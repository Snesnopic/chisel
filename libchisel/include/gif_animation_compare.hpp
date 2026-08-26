//
// Created by Giuseppe Francione on 26/08/26.
//

/**
 * @file gif_animation_compare.hpp
 * @brief Compares two GIFs by their played timeline instead of their raw frame arrays.
 */

#ifndef CHISEL_GIF_ANIMATION_COMPARE_HPP
#define CHISEL_GIF_ANIMATION_COMPARE_HPP

#include <span>
#include <string>

namespace chisel {

/**
 * @brief Result of gif_animations_equal(): whether the two animations play
 * back identically, and (when they don't) why.
 */
struct GifCompareResult {
    bool equal = false;
    std::string reason; // empty when equal
};

/**
 * @brief Compares two GIFs by the sequence of distinct canvases they render
 * and how long each is shown, not by their raw stb_image frame arrays.
 *
 * @details A recompressor is allowed to merge consecutive frames that render
 * the same canvas into one, summing their delays -- that's legitimate L2
 * compression, not data loss, so comparing raw frame counts (as a naive
 * stbi_load_gif_from_memory-based check would) rejects valid output.
 * Decoding is streamed one canvas at a time via stb's internal frame-by-frame
 * GIF API instead of stb_image's usual whole-animation-in-one-buffer
 * approach, so memory stays bounded regardless of frame count (a large
 * animation can otherwise need several GB in one allocation).
 *
 * @param a First GIF's raw file bytes.
 * @param b Second GIF's raw file bytes.
 * @return equal=true only if both sides play back the exact same sequence of
 * canvases with the exact same per-moment durations; false (with a reason)
 * on any mismatch, decode failure, or invalid input.
 */
GifCompareResult gif_animations_equal(std::span<const unsigned char> a, std::span<const unsigned char> b);

} // namespace chisel

#endif //CHISEL_GIF_ANIMATION_COMPARE_HPP
