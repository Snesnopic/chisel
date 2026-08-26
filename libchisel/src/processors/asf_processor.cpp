//
// Created by Giuseppe Francione on 25/08/26.
//

#include "../../include/asf_processor.hpp"
#include "../../include/logger.hpp"
#include "../../include/audio_metadata_util.hpp"
#include "../../include/sparse_rewrite_util.hpp"
#include "file_utils.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace chisel {
namespace fs = std::filesystem;

namespace {

using Guid = std::array<uint8_t, 16>;

// Parses a GUID's standard string form ("XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX")
// into its on-disk byte layout: Data1/Data2/Data3 are little-endian, Data4 is
// a plain 8-byte array. Runtime, not constexpr -- called once per constant.
Guid parse_guid(const std::string_view s) {
    // "XXXXXXXX-XXXX-XXXX-YYYY-ZZZZZZZZZZZZ": Data4 is split by a dash after
    // its first 2 bytes (position 19-22), then continues unbroken (24-35).
    auto hex = [&](const size_t pos, const int len) {
        return static_cast<uint32_t>(std::stoul(std::string(s.substr(pos, len)), nullptr, 16));
    };
    Guid g{};
    const uint32_t d1 = hex(0, 8);
    const uint32_t d2 = hex(9, 4);
    const uint32_t d3 = hex(14, 4);
    g[0] = d1 & 0xFF; g[1] = (d1 >> 8) & 0xFF; g[2] = (d1 >> 16) & 0xFF; g[3] = (d1 >> 24) & 0xFF;
    g[4] = d2 & 0xFF; g[5] = (d2 >> 8) & 0xFF;
    g[6] = d3 & 0xFF; g[7] = (d3 >> 8) & 0xFF;
    g[8] = hex(19, 2);
    g[9] = hex(21, 2);
    for (int i = 0; i < 6; ++i) {
        g[10 + i] = hex(24 + i * 2, 2);
    }
    return g;
}

const Guid kHeaderObject = parse_guid("75B22630-668E-11CF-A6D9-00AA0062CE6C");
const Guid kDataObject = parse_guid("75B22636-668E-11CF-A6D9-00AA0062CE6C");
const Guid kHeaderExtensionObject = parse_guid("5FBF03B5-A92E-11CF-8EE3-00C00C205365");
const Guid kPaddingObject = parse_guid("1806D474-CADF-4509-A4BA-9AABCB96AAE8");
const Guid kContentDescriptionObject = parse_guid("75B22633-668E-11CF-A6D9-00AA0062CE6C");
const Guid kExtendedContentDescriptionObject = parse_guid("D2D0A440-E307-11D2-97F0-00A0C95EA850");

// ASF object: [16B GUID][8B QWORD size][payload]. Object Size includes this
// 24-byte header. Both container types this processor descends into (Header
// Object, Header Extension Object) have extra fixed fields between this base
// header and their first child, handled by the caller.
struct AsfObject {
    Guid guid{};
    uint64_t start = 0;
    uint64_t total_size = 0;

    [[nodiscard]] uint64_t end() const { return start + total_size; }
};

std::vector<AsfObject> parse_objects(std::ifstream& in, const uint64_t range_start, const uint64_t range_end) {
    std::vector<AsfObject> objects;
    uint64_t pos = range_start;

    while (pos + 24 <= range_end) {
        uint8_t hdr[24];
        in.seekg(static_cast<std::streamoff>(pos));
        in.read(reinterpret_cast<char*>(hdr), 24);
        if (!in) throw std::runtime_error("asf: truncated object header");

        AsfObject obj;
        obj.start = pos;
        std::memcpy(obj.guid.data(), hdr, 16);
        obj.total_size = read_le64(hdr + 16);

        if (obj.total_size < 24 || pos + obj.total_size > range_end) {
            throw std::runtime_error("asf: object size out of range");
        }

        objects.push_back(obj);
        pos += obj.total_size;
    }

    return objects;
}

struct AsfCleanupPlan {
    std::vector<std::pair<uint64_t, uint64_t>> removed_ranges; // (start, length)

    // (position, new value, width in bytes: 4 for DWORD, 8 for QWORD)
    std::vector<std::tuple<uint64_t, uint64_t, int>> field_patches;

    void scan(std::ifstream& in, const uint64_t file_size, const bool preserve_metadata) {
        const auto top = parse_objects(in, 0, file_size);
        for (const auto& obj : top) {
            if (obj.guid == kHeaderObject) {
                scan_header(in, obj, preserve_metadata);
            }
            // Data Object and any Index Object(s): left completely untouched.
        }
        std::sort(removed_ranges.begin(), removed_ranges.end());
    }

private:
    void remove(const AsfObject& obj) {
        removed_ranges.emplace_back(obj.start, obj.total_size);
    }

    // Header Object: [24B base header][4B Number of Header Objects][1B][1B][children...]
    void scan_header(std::ifstream& in, const AsfObject& header, const bool preserve_metadata) {
        static constexpr uint64_t kFixedFields = 24 + 4 + 1 + 1;
        const uint64_t children_start = header.start + kFixedFields;
        const auto children = parse_objects(in, children_start, header.end());

        uint64_t removed_within = 0;
        uint32_t removed_count = 0;
        for (const auto& child : children) {
            if (child.guid == kPaddingObject) {
                remove(child);
                removed_within += child.total_size;
                removed_count++;
            } else if (!preserve_metadata &&
                       (child.guid == kContentDescriptionObject || child.guid == kExtendedContentDescriptionObject)) {
                remove(child);
                removed_within += child.total_size;
                removed_count++;
            } else if (child.guid == kHeaderExtensionObject) {
                scan_header_extension(in, child);
            }
        }

        if (removed_within > 0) {
            field_patches.emplace_back(header.start + 16, header.total_size - removed_within, 8);
        }
        if (removed_count > 0) {
            uint8_t buf[4];
            in.seekg(static_cast<std::streamoff>(header.start + 24));
            in.read(reinterpret_cast<char*>(buf), 4);
            if (!in) throw std::runtime_error("asf: truncated Number of Header Objects field");
            const uint32_t original_count = read_le32(buf);
            field_patches.emplace_back(header.start + 24, original_count - removed_count, 4);
        }
    }

    // Header Extension Object: [24B base header][16B Reserved1][2B Reserved2][4B Header Extension Data Size][children...]
    void scan_header_extension(std::ifstream& in, const AsfObject& hdrext) {
        static constexpr uint64_t kFixedFields = 24 + 16 + 2 + 4;
        const uint64_t children_start = hdrext.start + kFixedFields;
        const auto children = parse_objects(in, children_start, hdrext.end());

        uint64_t removed_within = 0;
        for (const auto& child : children) {
            if (child.guid == kPaddingObject) {
                remove(child);
                removed_within += child.total_size;
            }
        }

        if (removed_within > 0) {
            field_patches.emplace_back(hdrext.start + 16, hdrext.total_size - removed_within, 8);
            field_patches.emplace_back(hdrext.start + 40, (hdrext.total_size - kFixedFields) - removed_within, 4);
        }
    }
};

void apply_patches(const fs::path& output, const AsfCleanupPlan& plan) {
    std::fstream out(output, std::ios::binary | std::ios::in | std::ios::out);
    if (!out) throw std::runtime_error("asf: cannot reopen output for patching");

    for (const auto& [pos, value, width] : plan.field_patches) {
        uint8_t buf[8];
        if (width == 8) write_le64(buf, value);
        else write_le32(buf, static_cast<uint32_t>(value));
        out.seekp(static_cast<std::streamoff>(SparseRewriteUtil::shift(plan.removed_ranges, pos)));
        out.write(reinterpret_cast<char*>(buf), width);
    }
    if (!out) throw std::runtime_error("asf: failed while patching sizes");
}

// Byte range of the top-level Data Object, used by raw_equal().
std::pair<uint64_t, uint64_t> find_data_object(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("asf: cannot open " + path.string());
    in.seekg(0, std::ios::end);
    const auto file_size = static_cast<uint64_t>(in.tellg());
    in.seekg(0);

    const auto top = parse_objects(in, 0, file_size);
    for (const auto& obj : top) {
        if (obj.guid == kDataObject) return {obj.start, obj.total_size};
    }
    throw std::runtime_error("asf: no Data Object found in " + path.string());
}

bool data_object_bytes_equal(const fs::path& a, const fs::path& b) {
    const auto [start_a, size_a] = find_data_object(a);
    const auto [start_b, size_b] = find_data_object(b);
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

void AsfProcessor::recompress(const fs::path& input,
                              const fs::path& output, const ProcessingOptions &options) {
    Logger::log(LogLevel::Debug, "Entering recompress for " + input.string(), get_name());

    std::ifstream in(input, std::ios::binary);
    if (!in) throw std::runtime_error("AsfProcessor: cannot open " + input.string());
    in.seekg(0, std::ios::end);
    const auto file_size = static_cast<uint64_t>(in.tellg());
    in.seekg(0);

    AsfCleanupPlan plan;
    plan.scan(in, file_size, options.preserve_metadata);

    if (plan.removed_ranges.empty()) {
        AudioMetadataUtil::placeholderCopyRecompress(input, output, get_name());
        Logger::log(LogLevel::Debug, "Nothing to strip for " + input.string(), get_name());
        return;
    }

    {
        std::ofstream out(output, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("AsfProcessor: cannot open " + output.string() + " for writing");
        SparseRewriteUtil::stream_copy_with_skips(in, out, file_size, plan.removed_ranges);
    }
    apply_patches(output, plan);

    Logger::log(LogLevel::Debug, "Exiting recompress for " + output.string(), get_name());
}

std::optional<ExtractedContent> AsfProcessor::prepare_extraction(const fs::path& input_path) {
    return AudioMetadataUtil::prepareCoverExtraction(input_path, "asf-processor", get_name());
}

fs::path AsfProcessor::finalize_extraction(const ExtractedContent &content, const ProcessingOptions &options) {
    return AudioMetadataUtil::finalizeCoverExtraction(content, get_name());
}

std::string AsfProcessor::get_raw_checksum(const fs::path&) const {
    return "";
}

bool AsfProcessor::raw_equal(const fs::path& a, const fs::path& b) const {
    try {
        return data_object_bytes_equal(a, b);
    } catch (const std::exception& e) {
        Logger::log(LogLevel::Warning, std::string("raw_equal: ") + e.what(), get_name());
        return false;
    }
}

} // namespace chisel
