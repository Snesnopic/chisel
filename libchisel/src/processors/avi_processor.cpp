//
// Created by Giuseppe Francione on 26/08/26.
//

#include "../../include/avi_processor.hpp"
#include "../../include/logger.hpp"
#include "../../include/sparse_rewrite_util.hpp"
#include "file_utils.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace chisel {
namespace fs = std::filesystem;

namespace {

// RIFF entry: either a plain chunk [4B ckID][4B ckSize LE][ckData, padded to
// an even boundary] or a LIST [4B 'LIST'][4B listSize LE][4B listType]
// [listData, padded]. ckSize/listSize never counts the padding byte, but the
// next entry still starts after it -- total_size below always includes it.
struct TopEntry {
    bool is_list = false;
    std::string id;          // ckID for a plain chunk, listType for a LIST
    uint64_t start = 0;      // position of ckID (or the 'LIST' fourcc)
    uint64_t total_size = 0; // full on-disk size, header(s) + payload + padding
    uint64_t data_start = 0; // where ckData/listData begins
    uint64_t data_size = 0;  // ckSize, or listSize - 4 (excludes listType)
};

std::vector<TopEntry> parse_top_entries(std::ifstream& in, const uint64_t range_start, const uint64_t range_end) {
    std::vector<TopEntry> entries;
    uint64_t pos = range_start;

    while (pos + 8 <= range_end) {
        uint8_t hdr[12];
        in.seekg(static_cast<std::streamoff>(pos));
        in.read(reinterpret_cast<char*>(hdr), 8);
        if (!in) throw std::runtime_error("avi: truncated chunk header");

        const std::string id0(reinterpret_cast<char*>(hdr), 4);
        const uint32_t size = read_le32(hdr + 4);

        TopEntry e;
        e.start = pos;

        if (id0 == "LIST") {
            if (pos + 12 > range_end || size < 4) throw std::runtime_error("avi: truncated LIST header");
            in.read(reinterpret_cast<char*>(hdr + 8), 4);
            if (!in) throw std::runtime_error("avi: truncated LIST header");
            e.is_list = true;
            e.id.assign(reinterpret_cast<char*>(hdr + 8), 4);
            e.data_start = pos + 12;
            e.data_size = size - 4;
        } else {
            e.is_list = false;
            e.id = id0;
            e.data_start = pos + 8;
            e.data_size = size;
        }

        // 'size' is ckSize (plain chunk) or listSize (LIST: listType + listData);
        // either way the on-disk total is the 8-byte fourcc+size header plus
        // 'size' bytes, padded to an even boundary.
        const uint64_t padded = static_cast<uint64_t>(size) + (size & 1);
        e.total_size = 8 + padded;
        if (pos + e.total_size > range_end) throw std::runtime_error("avi: entry '" + e.id + "' size out of range");

        entries.push_back(e);
        pos += e.total_size;
    }

    return entries;
}

class AviCleanupPlan {
public:
    std::vector<std::pair<uint64_t, uint64_t>> removed_ranges;         // (start, length)
    std::vector<std::pair<uint64_t, uint64_t>> field_patches;          // (position, new DWORD value)

    void scan(std::ifstream& in, const uint64_t file_size, const bool preserve_metadata) {
        uint8_t hdr[12];
        in.seekg(0);
        in.read(reinterpret_cast<char*>(hdr), 12);
        if (!in || std::memcmp(hdr, "RIFF", 4) != 0 || std::memcmp(hdr + 8, "AVI ", 4) != 0) {
            throw std::runtime_error("avi: not an AVI RIFF file");
        }
        const uint32_t riff_payload_size = read_le32(hdr + 4);

        const uint64_t top_end = std::min<uint64_t>(8ULL + riff_payload_size, file_size);
        const auto entries = parse_top_entries(in, 12, top_end);

        const TopEntry* movi = nullptr;
        const TopEntry* idx1 = nullptr;
        const TopEntry* hdrl = nullptr;
        for (const auto& e : entries) {
            if (e.is_list && e.id == "movi") movi = &e;
            else if (!e.is_list && e.id == "idx1") idx1 = &e;
            else if (e.is_list && e.id == "hdrl") hdrl = &e;
        }
        if (!movi) {
            Logger::log(LogLevel::Debug, "avi: no 'movi' list, nothing safe to do", "AviProcessor");
            return;
        }

        for (const auto& e : entries) {
            if (!e.is_list && e.id == "JUNK") {
                remove(e);
            } else if (e.is_list && e.id == "INFO" && !preserve_metadata) {
                remove(e);
            }
        }
        if (hdrl) {
            uint64_t removed_within_hdrl = 0;
            for (const auto& c : parse_top_entries(in, hdrl->data_start, hdrl->data_start + hdrl->data_size)) {
                if (!c.is_list && c.id == "JUNK") {
                    remove(c);
                    removed_within_hdrl += c.total_size;
                }
            }
            if (removed_within_hdrl > 0) {
                // hdrl's own listSize field (4 bytes right after the 'LIST' fourcc)
                // includes its 4-byte listType plus all of listData.
                field_patches.emplace_back(hdrl->start + 4, (4 + hdrl->data_size) - removed_within_hdrl);
            }
        }

        if (removed_ranges.empty()) return;
        std::sort(removed_ranges.begin(), removed_ranges.end());

        uint64_t removed_before_movi = 0;
        for (const auto& [start, length] : removed_ranges) {
            if (start < movi->start) removed_before_movi += length;
        }

        if (removed_before_movi > 0 && idx1 != nullptr) {
            if (!calibrate_and_plan_idx1(in, *movi, *idx1)) {
                Logger::log(LogLevel::Warning,
                            "avi: idx1 offset convention could not be confidently determined, skipping cleanup",
                            "AviProcessor");
                removed_ranges.clear();
                field_patches.clear();
                return;
            }
        }

        uint64_t total_removed = 0;
        for (const auto& [start, length] : removed_ranges) total_removed += length;
        field_patches.emplace_back(4, riff_payload_size - total_removed);
    }

private:
    void remove(const TopEntry& e) { removed_ranges.emplace_back(e.start, e.total_size); }

    // Returns false only when the file's idx1 convention can't be confidently
    // determined; the caller then abandons the whole cleanup for this file
    // rather than risk writing a corrupted index. See avi_processor.hpp for
    // the calibration idea, adapted from ffmpeg's avi_read_idx1().
    bool calibrate_and_plan_idx1(std::ifstream& in, const TopEntry& movi, const TopEntry& idx1) {
        if (idx1.data_size < 16) return true; // no real entries: nothing to break

        uint64_t true_first = movi.data_start;
        while (true_first + 8 <= movi.data_start + movi.data_size) {
            uint8_t hdr[8];
            in.seekg(static_cast<std::streamoff>(true_first));
            in.read(reinterpret_cast<char*>(hdr), 8);
            if (!in) break;
            if (std::memcmp(hdr, "JUNK", 4) != 0) break;
            const uint32_t size = read_le32(hdr + 4);
            true_first += 8 + size + (size & 1);
        }

        uint8_t entry[16];
        in.seekg(static_cast<std::streamoff>(idx1.data_start));
        in.read(reinterpret_cast<char*>(entry), 16);
        if (!in) return false;
        const uint32_t raw_first = read_le32(entry + 8);

        const auto calibration = static_cast<int64_t>(true_first) - static_cast<int64_t>(raw_first);
        constexpr int64_t kTolerance = 512;

        bool absolute_convention;
        if (std::llabs(calibration) < kTolerance) {
            absolute_convention = true;
        } else if (std::llabs(calibration - static_cast<int64_t>(movi.data_start)) < kTolerance) {
            absolute_convention = false;
        } else {
            return false;
        }

        if (!absolute_convention) return true; // movi-relative: untouched movi means untouched offsets

        const auto count = static_cast<uint32_t>(idx1.data_size / 16);
        for (uint32_t i = 0; i < count; ++i) {
            const uint64_t field_pos = idx1.data_start + static_cast<uint64_t>(i) * 16 + 8;
            uint8_t buf[4];
            in.seekg(static_cast<std::streamoff>(field_pos));
            in.read(reinterpret_cast<char*>(buf), 4);
            if (!in) return false;
            const uint32_t val = read_le32(buf);
            field_patches.emplace_back(field_pos, SparseRewriteUtil::shift(removed_ranges, val));
        }
        return true;
    }
};

void apply_patches(const fs::path& output, const AviCleanupPlan& plan) {
    std::fstream out(output, std::ios::binary | std::ios::in | std::ios::out);
    if (!out) throw std::runtime_error("avi: cannot reopen output for patching");

    for (const auto& [pos, value] : plan.field_patches) {
        uint8_t buf[4];
        write_le32(buf, static_cast<uint32_t>(value));
        out.seekp(static_cast<std::streamoff>(SparseRewriteUtil::shift(plan.removed_ranges, pos)));
        out.write(reinterpret_cast<char*>(buf), 4);
    }
    if (!out) throw std::runtime_error("avi: failed while patching");
}

// Byte range of the top-level 'movi' LIST's payload, used by raw_equal().
std::pair<uint64_t, uint64_t> find_movi(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("avi: cannot open " + path.string());
    in.seekg(0, std::ios::end);
    const auto file_size = static_cast<uint64_t>(in.tellg());
    in.seekg(0);

    uint8_t hdr[12];
    in.read(reinterpret_cast<char*>(hdr), 12);
    if (!in || std::memcmp(hdr, "RIFF", 4) != 0) throw std::runtime_error("avi: not a RIFF file");
    const uint32_t riff_payload_size = read_le32(hdr + 4);
    const uint64_t top_end = std::min<uint64_t>(8ULL + riff_payload_size, file_size);

    for (const auto& e : parse_top_entries(in, 12, top_end)) {
        if (e.is_list && e.id == "movi") return {e.data_start, e.data_size};
    }
    throw std::runtime_error("avi: no 'movi' list found in " + path.string());
}

bool movi_bytes_equal(const fs::path& a, const fs::path& b) {
    const auto [start_a, size_a] = find_movi(a);
    const auto [start_b, size_b] = find_movi(b);
    if (size_a != size_b) return false;

    std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
    if (!fa || !fb) return false;
    fa.seekg(static_cast<std::streamoff>(start_a));
    fb.seekg(static_cast<std::streamoff>(start_b));

    std::vector<char> bufa(1 << 20), bufb(1 << 20);
    uint64_t left = size_a;
    while (left > 0) {
        const auto chunk = static_cast<std::streamsize>(std::min<uint64_t>(bufa.size(), left));
        fa.read(bufa.data(), chunk);
        fb.read(bufb.data(), chunk);
        if (!fa || !fb) return false;
        if (std::memcmp(bufa.data(), bufb.data(), chunk) != 0) return false;
        left -= static_cast<uint64_t>(chunk);
    }
    return true;
}

} // namespace

void AviProcessor::recompress(const fs::path& input,
                              const fs::path& output, const ProcessingOptions &options) {
    Logger::log(LogLevel::Debug, "Entering recompress for " + input.string(), get_name());

    std::ifstream in(input, std::ios::binary);
    if (!in) throw std::runtime_error("AviProcessor: cannot open " + input.string());
    in.seekg(0, std::ios::end);
    const auto file_size = static_cast<uint64_t>(in.tellg());
    in.seekg(0);

    AviCleanupPlan plan;
    plan.scan(in, file_size, options.preserve_metadata);

    if (plan.removed_ranges.empty()) {
        std::ifstream src(input, std::ios::binary);
        std::ofstream dst(output, std::ios::binary | std::ios::trunc);
        dst << src.rdbuf();
        Logger::log(LogLevel::Debug, "Nothing to strip for " + input.string(), get_name());
        return;
    }

    {
        std::ofstream out(output, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("AviProcessor: cannot open " + output.string() + " for writing");
        SparseRewriteUtil::stream_copy_with_skips(in, out, file_size, plan.removed_ranges);
    }
    apply_patches(output, plan);

    Logger::log(LogLevel::Debug, "Exiting recompress for " + output.string(), get_name());
}

std::string AviProcessor::get_raw_checksum(const fs::path&) const {
    return "";
}

bool AviProcessor::raw_equal(const fs::path& a, const fs::path& b) const {
    try {
        return movi_bytes_equal(a, b);
    } catch (const std::exception& e) {
        Logger::log(LogLevel::Warning, std::string("raw_equal: ") + e.what(), get_name());
        return false;
    }
}

} // namespace chisel
