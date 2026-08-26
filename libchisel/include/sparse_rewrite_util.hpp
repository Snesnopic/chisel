//
// Created by Giuseppe Francione on 26/08/26.
//

/**
 * @file sparse_rewrite_util.hpp
 * @brief Shared helpers for rewriting a file with some byte ranges removed.
 */

#ifndef CHISEL_SPARSE_REWRITE_UTIL_HPP
#define CHISEL_SPARSE_REWRITE_UTIL_HPP

#include <cstdint>
#include <fstream>
#include <utility>
#include <vector>

namespace chisel {

/**
 * @brief Shared byte-range-removal helpers for container processors
 * (Mp4Processor, AsfProcessor, AviProcessor) that strip specific spans from
 * a file (padding, metadata) and stream-copy the rest through unchanged.
 *
 * @details ranges must be sorted by start and non-overlapping, as produced
 * by std::sort on a plain vector of (start, length) pairs.
 */
class SparseRewriteUtil {
public:
    /**
     * @brief Maps a position in the original file to its position in the
     * rewritten file: original_pos minus every removed range strictly
     * before it. Also used to remap values that are themselves absolute
     * file offsets (e.g. MP4 stco/co64, AVI idx1), since removed ranges
     * never point inside data those values reference.
     * @param removed_ranges Sorted, non-overlapping (start, length) pairs.
     * @param original_pos Position (or offset value) in the original file.
     * @return The corresponding position (or value) after removal.
     */
    static uint64_t shift(const std::vector<std::pair<uint64_t, uint64_t>>& removed_ranges,
                          uint64_t original_pos);

    /**
     * @brief Copies the whole file from in to out, skipping every byte range
     * in removed_ranges.
     * @param in Source stream, seekable.
     * @param out Destination stream, positioned at its start.
     * @param file_size Total size of the source.
     * @param removed_ranges Sorted, non-overlapping (start, length) pairs.
     * @throws std::runtime_error if a read fails partway through.
     */
    static void stream_copy_with_skips(std::ifstream& in, std::ofstream& out, uint64_t file_size,
                                       const std::vector<std::pair<uint64_t, uint64_t>>& removed_ranges);
};

} // namespace chisel

#endif //CHISEL_SPARSE_REWRITE_UTIL_HPP
