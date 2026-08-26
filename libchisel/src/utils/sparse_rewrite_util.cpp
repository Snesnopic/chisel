//
// Created by Giuseppe Francione on 26/08/26.
//

#include "../../include/sparse_rewrite_util.hpp"
#include <algorithm>
#include <stdexcept>

namespace chisel {

uint64_t SparseRewriteUtil::shift(const std::vector<std::pair<uint64_t, uint64_t>>& removed_ranges,
                                  const uint64_t original_pos) {
    uint64_t total = 0;
    for (const auto& [start, length] : removed_ranges) {
        if (start < original_pos) total += length;
        else break;
    }
    return original_pos - total;
}

void SparseRewriteUtil::stream_copy_with_skips(std::ifstream& in, std::ofstream& out, const uint64_t file_size,
                                               const std::vector<std::pair<uint64_t, uint64_t>>& removed_ranges) {
    std::vector<char> buffer(1 << 20);
    uint64_t pos = 0;

    auto copy_range = [&](uint64_t from, const uint64_t to) {
        in.seekg(static_cast<std::streamoff>(from));
        while (from < to) {
            const auto chunk = static_cast<std::streamsize>(std::min<uint64_t>(buffer.size(), to - from));
            in.read(buffer.data(), chunk);
            if (!in) throw std::runtime_error("sparse_rewrite: read failed while copying");
            out.write(buffer.data(), chunk);
            from += static_cast<uint64_t>(chunk);
        }
    };

    for (const auto& [start, length] : removed_ranges) {
        copy_range(pos, start);
        pos = start + length;
    }
    copy_range(pos, file_size);
}

} // namespace chisel
