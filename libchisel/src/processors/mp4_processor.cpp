//
// Created by Giuseppe Francione on 25/08/26.
//

#include "../../include/mp4_processor.hpp"
#include "../../include/logger.hpp"
#include "../../include/audio_metadata_util.hpp"
#include "../../include/sparse_rewrite_util.hpp"
#include "file_utils.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace chisel {
namespace fs = std::filesystem;

namespace {

// ISOBMFF box: [4B size][4B fourcc][8B largesize if size==1][payload...].
// size==0 means "extends to the end of the enclosing range" (only meaningful
// for the last box in a range, e.g. a trailing mdat); such boxes carry no
// size field to patch, so they're never added to size_decrements.
struct Box {
    std::string fourcc;
    uint64_t start = 0;         // absolute offset of the box header
    uint64_t header_size = 0;   // 8 (short) or 16 (size==1, largesize follows)
    uint64_t total_size = 0;    // header + payload
    bool explicit_size = true;  // false if the on-disk size field was 0

    [[nodiscard]] uint64_t payload_start() const { return start + header_size; }
    [[nodiscard]] uint64_t payload_size() const {
        return total_size > header_size ? total_size - header_size : 0;
    }
};

std::vector<Box> parse_boxes(std::ifstream& in, const uint64_t range_start, const uint64_t range_end) {
    std::vector<Box> boxes;
    uint64_t pos = range_start;

    while (pos + 8 <= range_end) {
        uint8_t hdr[16];
        in.seekg(static_cast<std::streamoff>(pos));
        in.read(reinterpret_cast<char*>(hdr), 8);
        if (!in) throw std::runtime_error("mp4: truncated box header");

        Box box;
        box.start = pos;
        const uint32_t size32 = read_be32(hdr);
        box.fourcc.assign(reinterpret_cast<char*>(hdr + 4), 4);

        if (size32 == 1) {
            if (pos + 16 > range_end) throw std::runtime_error("mp4: truncated largesize box header");
            in.read(reinterpret_cast<char*>(hdr + 8), 8);
            if (!in) throw std::runtime_error("mp4: truncated largesize box header");
            box.header_size = 16;
            box.total_size = read_be64(hdr + 8);
        } else if (size32 == 0) {
            box.header_size = 8;
            box.total_size = range_end - pos;
            box.explicit_size = false;
        } else {
            box.header_size = 8;
            box.total_size = size32;
        }

        if (box.total_size < box.header_size || pos + box.total_size > range_end) {
            throw std::runtime_error("mp4: box '" + box.fourcc + "' size out of range");
        }

        boxes.push_back(box);
        pos += box.total_size;
        if (!box.explicit_size) break; // consumed the rest of the range
    }

    return boxes;
}

// Plans every byte-range removal and offset patch needed to strip padding
// (and, unless preserving metadata, udta/meta) from an MP4/QuickTime file.
class Mp4CleanupPlan {
public:
    bool has_fragments = false;
    std::vector<std::pair<uint64_t, uint64_t>> removed_ranges; // (start, length), unsorted until finalize()
    std::vector<std::pair<uint64_t, uint64_t>> size_patches;   // (size-field pos, new value)
    std::vector<bool> size_patch_is64;                         // parallel to size_patches

    struct StcoInfo {
        uint64_t entries_pos = 0;
        uint32_t entry_count = 0;
        bool is64 = false;
    };
    std::vector<StcoInfo> stco_list;

    void scan(std::ifstream& in, uint64_t file_size, bool preserve_metadata) {
        preserve_metadata_ = preserve_metadata;

        const auto top = parse_boxes(in, 0, file_size);
        for (const auto& box : top) {
            if (box.fourcc == "moof") has_fragments = true;
        }
        if (has_fragments) return;

        std::vector<Box> ancestors;
        for (const auto& box : top) {
            if (is_padding(box.fourcc)) {
                remove_box(box, ancestors);
            } else if (box.fourcc == "meta" && !preserve_metadata_) {
                remove_box(box, ancestors);
            } else if (box.fourcc == "moov") {
                ancestors.push_back(box);
                scan_moov(in, box, ancestors);
                ancestors.pop_back();
            }
            // ftyp, mdat, and anything unrecognized: left untouched.
        }

        finalize();
    }

private:
    bool preserve_metadata_ = true;
    std::map<uint64_t, uint64_t> size_decrements_; // keyed by ancestor.start
    std::map<uint64_t, Box> container_by_start_;

    static bool is_padding(const std::string& fourcc) {
        return fourcc == "free" || fourcc == "skip" || fourcc == "wide";
    }

    void remove_box(const Box& box, const std::vector<Box>& ancestors) {
        removed_ranges.emplace_back(box.start, box.total_size);
        for (const auto& ancestor : ancestors) {
            size_decrements_[ancestor.start] += box.total_size;
            container_by_start_.emplace(ancestor.start, ancestor);
        }
    }

    void scan_moov(std::ifstream& in, const Box& moov, std::vector<Box>& ancestors) {
        const auto children = parse_boxes(in, moov.payload_start(), moov.payload_start() + moov.payload_size());
        for (const auto& child : children) {
            if (is_padding(child.fourcc)) {
                remove_box(child, ancestors);
            } else if ((child.fourcc == "udta" || child.fourcc == "meta") && !preserve_metadata_) {
                remove_box(child, ancestors);
            } else if (child.fourcc == "trak") {
                ancestors.push_back(child);
                scan_trak(in, child, ancestors);
                ancestors.pop_back();
            }
        }
    }

    void scan_trak(std::ifstream& in, const Box& trak, std::vector<Box>& ancestors) {
        const auto children = parse_boxes(in, trak.payload_start(), trak.payload_start() + trak.payload_size());
        for (const auto& child : children) {
            if (is_padding(child.fourcc)) {
                remove_box(child, ancestors);
            } else if (child.fourcc == "udta" && !preserve_metadata_) {
                remove_box(child, ancestors);
            } else if (child.fourcc == "mdia") {
                ancestors.push_back(child);
                scan_mdia(in, child, ancestors);
                ancestors.pop_back();
            }
        }
    }

    void scan_mdia(std::ifstream& in, const Box& mdia, std::vector<Box>& ancestors) {
        const auto children = parse_boxes(in, mdia.payload_start(), mdia.payload_start() + mdia.payload_size());
        for (const auto& child : children) {
            if (child.fourcc == "minf") {
                ancestors.push_back(child);
                scan_minf(in, child, ancestors);
                ancestors.pop_back();
            }
        }
    }

    void scan_minf(std::ifstream& in, const Box& minf, std::vector<Box>& ancestors) {
        const auto children = parse_boxes(in, minf.payload_start(), minf.payload_start() + minf.payload_size());
        for (const auto& child : children) {
            if (child.fourcc == "stbl") {
                scan_stbl(in, child);
            }
        }
    }

    void scan_stbl(std::ifstream& in, const Box& stbl) {
        const auto children = parse_boxes(in, stbl.payload_start(), stbl.payload_start() + stbl.payload_size());
        for (const auto& child : children) {
            if (child.fourcc != "stco" && child.fourcc != "co64") continue;

            uint8_t hdr[8];
            in.seekg(static_cast<std::streamoff>(child.payload_start()));
            in.read(reinterpret_cast<char*>(hdr), 8);
            if (!in) throw std::runtime_error("mp4: truncated " + child.fourcc + " header");

            StcoInfo info;
            info.entries_pos = child.payload_start() + 8;
            info.entry_count = read_be32(hdr + 4);
            info.is64 = (child.fourcc == "co64");
            stco_list.push_back(info);
        }
    }

    void finalize() {
        std::sort(removed_ranges.begin(), removed_ranges.end());

        for (const auto& [start, decrement] : size_decrements_) {
            const Box& box = container_by_start_.at(start);
            if (!box.explicit_size) continue; // size==0: self-correcting via truncation
            const uint64_t new_size = box.total_size - decrement;
            if (box.header_size == 16) {
                size_patches.emplace_back(box.start + 8, new_size);
                size_patch_is64.push_back(true);
            } else {
                size_patches.emplace_back(box.start, new_size);
                size_patch_is64.push_back(false);
            }
        }
    }
};

void apply_patches(const fs::path& output, const Mp4CleanupPlan& plan) {
    std::fstream out(output, std::ios::binary | std::ios::in | std::ios::out);
    if (!out) throw std::runtime_error("mp4: cannot reopen output for patching");

    for (size_t i = 0; i < plan.size_patches.size(); ++i) {
        const auto [pos, value] = plan.size_patches[i];
        uint8_t buf[8];
        if (plan.size_patch_is64[i]) write_be64(buf, value);
        else write_be32(buf, static_cast<uint32_t>(value));
        out.seekp(static_cast<std::streamoff>(SparseRewriteUtil::shift(plan.removed_ranges, pos)));
        out.write(reinterpret_cast<char*>(buf), plan.size_patch_is64[i] ? 8 : 4);
    }

    for (const auto& stco : plan.stco_list) {
        const int width = stco.is64 ? 8 : 4;
        for (uint32_t i = 0; i < stco.entry_count; ++i) {
            const uint64_t entry_pos = stco.entries_pos + static_cast<uint64_t>(i) * width;
            out.seekg(static_cast<std::streamoff>(SparseRewriteUtil::shift(plan.removed_ranges, entry_pos)));
            uint8_t buf[8];
            out.read(reinterpret_cast<char*>(buf), width);
            const uint64_t value = stco.is64 ? read_be64(buf) : read_be32(buf);
            const uint64_t new_value = SparseRewriteUtil::shift(plan.removed_ranges, value);

            uint8_t out_buf[8];
            if (stco.is64) write_be64(out_buf, new_value);
            else write_be32(out_buf, static_cast<uint32_t>(new_value));
            out.seekp(static_cast<std::streamoff>(SparseRewriteUtil::shift(plan.removed_ranges, entry_pos)));
            out.write(reinterpret_cast<char*>(out_buf), width);
        }
    }
    if (!out) throw std::runtime_error("mp4: failed while patching offsets");
}

// Concatenated byte length of every top-level mdat, used by raw_equal().
struct MdatSummary {
    std::vector<std::pair<uint64_t, uint64_t>> ranges; // (payload_start, payload_size)
    uint32_t track_count = 0;
    std::vector<uint32_t> sample_counts;
};

MdatSummary summarize(const fs::path& path) {
    MdatSummary summary;
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("mp4: cannot open " + path.string());
    in.seekg(0, std::ios::end);
    const auto file_size = static_cast<uint64_t>(in.tellg());
    in.seekg(0);

    const auto top = parse_boxes(in, 0, file_size);
    for (const auto& box : top) {
        if (box.fourcc == "mdat") {
            summary.ranges.emplace_back(box.payload_start(), box.payload_size());
        } else if (box.fourcc == "moov") {
            const auto moov_children = parse_boxes(in, box.payload_start(), box.payload_start() + box.payload_size());
            for (const auto& mc : moov_children) {
                if (mc.fourcc != "trak") continue;
                summary.track_count++;
                const auto trak_children = parse_boxes(in, mc.payload_start(), mc.payload_start() + mc.payload_size());
                for (const auto& tc : trak_children) {
                    if (tc.fourcc != "mdia") continue;
                    const auto mdia_children = parse_boxes(in, tc.payload_start(), tc.payload_start() + tc.payload_size());
                    for (const auto& mdc : mdia_children) {
                        if (mdc.fourcc != "minf") continue;
                        const auto minf_children = parse_boxes(in, mdc.payload_start(), mdc.payload_start() + mdc.payload_size());
                        for (const auto& mnc : minf_children) {
                            if (mnc.fourcc != "stbl") continue;
                            const auto stbl_children = parse_boxes(in, mnc.payload_start(), mnc.payload_start() + mnc.payload_size());
                            for (const auto& sc : stbl_children) {
                                if (sc.fourcc != "stsz" && sc.fourcc != "stz2") continue;
                                uint8_t hdr[12];
                                in.seekg(static_cast<std::streamoff>(sc.payload_start()));
                                in.read(reinterpret_cast<char*>(hdr), 12);
                                if (!in) throw std::runtime_error("mp4: truncated stsz/stz2 header");
                                summary.sample_counts.push_back(read_be32(hdr + 8));
                            }
                        }
                    }
                }
            }
        }
    }
    return summary;
}

bool mdat_bytes_equal(const fs::path& a, const fs::path& b, const MdatSummary& sa, const MdatSummary& sb) {
    uint64_t total_a = 0, total_b = 0;
    for (const auto& [_, len] : sa.ranges) total_a += len;
    for (const auto& [_, len] : sb.ranges) total_b += len;
    if (total_a != total_b) return false;

    std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
    if (!fa || !fb) return false;

    std::vector<char> bufa(1 << 20), bufb(1 << 20);
    size_t ia = 0, ib = 0;
    uint64_t off_a = sa.ranges.empty() ? 0 : sa.ranges[0].first;
    uint64_t off_b = sb.ranges.empty() ? 0 : sb.ranges[0].first;
    uint64_t remaining_a = sa.ranges.empty() ? 0 : sa.ranges[0].second;
    uint64_t remaining_b = sb.ranges.empty() ? 0 : sb.ranges[0].second;
    uint64_t left = total_a;

    while (left > 0) {
        if (remaining_a == 0) {
            off_a = sa.ranges[++ia].first;
            remaining_a = sa.ranges[ia].second;
        }
        if (remaining_b == 0) {
            off_b = sb.ranges[++ib].first;
            remaining_b = sb.ranges[ib].second;
        }
        const auto chunk = std::min<uint64_t>({bufa.size(), remaining_a, remaining_b, left});
        fa.seekg(static_cast<std::streamoff>(off_a));
        fb.seekg(static_cast<std::streamoff>(off_b));
        fa.read(bufa.data(), static_cast<std::streamsize>(chunk));
        fb.read(bufb.data(), static_cast<std::streamsize>(chunk));
        if (!fa || !fb) return false;
        if (std::memcmp(bufa.data(), bufb.data(), chunk) != 0) return false;

        off_a += chunk; remaining_a -= chunk;
        off_b += chunk; remaining_b -= chunk;
        left -= chunk;
    }
    return true;
}

} // namespace

void Mp4Processor::recompress(const fs::path& input,
                              const fs::path& output, const ProcessingOptions &options) {
    Logger::log(LogLevel::Debug, "Entering recompress for " + input.string(), get_name());

    std::ifstream in(input, std::ios::binary);
    if (!in) throw std::runtime_error("Mp4Processor: cannot open " + input.string());
    in.seekg(0, std::ios::end);
    const auto file_size = static_cast<uint64_t>(in.tellg());
    in.seekg(0);

    Mp4CleanupPlan plan;
    plan.scan(in, file_size, options.preserve_metadata);

    if (plan.has_fragments) {
        Logger::log(LogLevel::Warning, "Fragmented MP4, skipping container cleanup: " + input.string(), get_name());
        AudioMetadataUtil::placeholderCopyRecompress(input, output, get_name());
        return;
    }

    if (plan.removed_ranges.empty()) {
        AudioMetadataUtil::placeholderCopyRecompress(input, output, get_name());
        Logger::log(LogLevel::Debug, "Nothing to strip for " + input.string(), get_name());
        return;
    }

    {
        std::ofstream out(output, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("Mp4Processor: cannot open " + output.string() + " for writing");
        SparseRewriteUtil::stream_copy_with_skips(in, out, file_size, plan.removed_ranges);
    }
    apply_patches(output, plan);

    Logger::log(LogLevel::Debug, "Exiting recompress for " + output.string(), get_name());
}

std::optional<ExtractedContent> Mp4Processor::prepare_extraction(const fs::path& input_path) {
    return AudioMetadataUtil::prepareCoverExtraction(input_path, "mp4-processor", get_name());
}

fs::path Mp4Processor::finalize_extraction(const ExtractedContent &content, const ProcessingOptions &options) {
    return AudioMetadataUtil::finalizeCoverExtraction(content, get_name());
}

std::string Mp4Processor::get_raw_checksum(const fs::path&) const {
    return "";
}

bool Mp4Processor::raw_equal(const fs::path& a, const fs::path& b) const {
    try {
        const auto sa = summarize(a);
        const auto sb = summarize(b);
        if (sa.track_count != sb.track_count) return false;
        if (sa.sample_counts != sb.sample_counts) return false;
        return mdat_bytes_equal(a, b, sa, sb);
    } catch (const std::exception& e) {
        Logger::log(LogLevel::Warning, std::string("raw_equal: ") + e.what(), get_name());
        return false;
    }
}

} // namespace chisel
