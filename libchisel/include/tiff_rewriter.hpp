//
// Created by Giuseppe Francione on 23/10/26.
//

/**
 * @file tiff_rewriter.hpp
 * @brief Rewrites TIFF files keeping their structure, recompressing only the image data.
 *
 * Every IFD entry keeps its tag, type, count and value bytes, sub-IFDs (EXIF, GPS, SubIFDs...)
 * included, in the file's own byte order and classic/BigTIFF layout. Only the image data of the IFDs
 * whose decoded samples can be carried over exactly changes, along with Compression, Predictor and
 * the offsets.
 */
#ifndef CHISEL_TIFF_REWRITER_HPP
#define CHISEL_TIFF_REWRITER_HPP

#include <filesystem>
#include <string>

namespace chisel {

struct ProcessingOptions;

/**
 * @brief Outcome of rewrite_tiff().
 */
struct TiffRewriteResult {
    bool written = false;   ///< the output holds the rewritten file
    bool mismatch = false;  ///< the rewritten file didn't read back as the original, and was discarded
    std::string reason;     ///< why nothing was written
};

/**
 * @brief Rewrites @p input to @p output, image data recompressed with Deflate where it gets smaller,
 * and checks that the result reads back as the original before keeping it.
 */
TiffRewriteResult rewrite_tiff(const std::filesystem::path& input, const std::filesystem::path& output,
                               const ProcessingOptions& options);

/**
 * @brief Compares two TIFFs: the same IFDs holding the same decoded image data and, with @p with_tags,
 * the same entries apart from those describing the image data's encoding and location.
 * @return Empty when they match, otherwise the first difference.
 */
std::string compare_tiff(const std::filesystem::path& a, const std::filesystem::path& b, bool with_tags);

} // namespace chisel

#endif // CHISEL_TIFF_REWRITER_HPP
