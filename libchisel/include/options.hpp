//
// Created by Giuseppe Francione on 24/02/26.
//

/**
 * @file options.hpp
 * @brief Utility definitions for OPTIONS.
 */

#ifndef CHISEL_OPTIONS_HPP
#define CHISEL_OPTIONS_HPP

#include <cstddef>

namespace chisel {

    /**
     * @brief Configuration context passed to processors during execution.
     */
    struct ProcessingOptions {
        size_t iterations = 15;                  /// Iteration count (Zopfli)
        size_t iterations_large = iterations / 3;/// Iteration count on large images (Zopfli)
        bool verify_checksums = false;           /// Verify original files are semantically equal to processed files
        bool preserve_metadata = true;           /// Don't discard metadata during recompression
        bool break_signatures = false;           /// Optimize digitally signed files too, invalidating their signatures
        bool keep_pixel_format = false;          /// Set by the executor, not by users: keep an image's color type and bit depth, which its container declares (e.g. a PDF image)
    };

} // namespace chisel

#endif // CHISEL_OPTIONS_HPP