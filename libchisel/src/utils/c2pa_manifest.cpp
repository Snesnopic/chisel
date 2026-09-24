//
// Created by Giuseppe Francione on 24/09/26.
//

#include "../../include/c2pa_manifest.hpp"
#include "../../include/file_utils.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace chisel::c2pa {

namespace fs = std::filesystem;

namespace {

// type of the jumbf description box of a manifest store: "c2pa" and the iso suffix
constexpr std::array<uint8_t, 16> kStoreType = {'c', '2', 'p', 'a', 0x00, 0x11, 0x00, 0x10,
                                                0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};

// bmff uuid box type chosen by c2pa
constexpr std::array<uint8_t, 16> kBmffUuid = {0xD8, 0xFE, 0xC3, 0xD6, 0x1B, 0x0E, 0x48, 0x3C,
                                               0x92, 0x97, 0x58, 0x28, 0x87, 0x7E, 0xC4, 0x81};

bool read_at(std::ifstream& in, const std::uint64_t pos, void* out, const std::size_t size) {
    in.clear();
    in.seekg(static_cast<std::streamoff>(pos));
    return static_cast<bool>(in.read(static_cast<char*>(out), static_cast<std::streamsize>(size)));
}

std::uint64_t stream_size(std::ifstream& in) {
    in.seekg(0, std::ios::end);
    const auto size = in.tellg();
    return size < 0 ? 0 : static_cast<std::uint64_t>(size);
}

struct Box {
    std::uint64_t pos = 0;
    std::uint64_t size = 0;   // header included
    std::uint32_t header = 8; // 16 with a 64-bit size
    std::string_view type() const { return {type_bytes.data(), type_bytes.size()}; }
    std::array<char, 4> type_bytes{};
};

// the box at pos, or nothing past the end or when its size is out of bounds
std::optional<Box> read_box(std::ifstream& in, const std::uint64_t pos, const std::uint64_t end) {
    std::array<uint8_t, 16> head{};
    if (pos >= end || end - pos < 8 || !read_at(in, pos, head.data(), 8)) return std::nullopt;
    Box box;
    box.pos = pos;
    box.size = read_be32(head.data());
    std::memcpy(box.type_bytes.data(), head.data() + 4, 4);
    if (box.size == 1) {
        if (end - pos < 16 || !read_at(in, pos + 8, head.data() + 8, 8)) return std::nullopt;
        box.size = read_be64(head.data() + 8);
        box.header = 16;
    } else if (box.size == 0) {
        box.size = end - pos;
    }
    if (box.size < box.header || box.size > end - pos) return std::nullopt;
    return box;
}

} // namespace

bool bmff_has_manifest(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    const auto end = stream_size(in);
    std::array<uint8_t, 16> uuid{};
    for (auto box = read_box(in, 0, end); box; box = read_box(in, box->pos + box->size, end)) {
        if (box->type() == "uuid" && box->size >= box->header + uuid.size() &&
            read_at(in, box->pos + box->header, uuid.data(), uuid.size()) && uuid == kBmffUuid) {
            return true;
        }
    }
    return false;
}

bool riff_has_manifest(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::array<uint8_t, 12> head{};
    if (!in.read(reinterpret_cast<char*>(head.data()), head.size()) || std::memcmp(head.data(), "RIFF", 4) != 0) {
        return false;
    }
    while (in.read(reinterpret_cast<char*>(head.data()), 8)) {
        if (std::memcmp(head.data(), "C2PA", 4) == 0) return true;
        const uint32_t size = read_le32(head.data() + 4);
        in.seekg(static_cast<std::streamoff>(size) + (size & 1), std::ios::cur);
    }
    return false;
}

bool tiff_has_manifest(const fs::path& path) {
    constexpr uint16_t kManifestTag = 52545;
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    const auto end = stream_size(in);
    std::array<uint8_t, 16> head{};
    if (end < 8 || !read_at(in, 0, head.data(), 8)) return false;
    const bool little = head[0] == 'I' && head[1] == 'I';
    if (!little && !(head[0] == 'M' && head[1] == 'M')) return false;
    const auto u16 = [little](const uint8_t* p) { return little ? read_le16(p) : read_be16(p); };
    const auto u32 = [little](const uint8_t* p) { return little ? read_le32(p) : read_be32(p); };
    const auto u64 = [little](const uint8_t* p) { return little ? read_le64(p) : read_be64(p); };

    const bool big = u16(head.data() + 2) == 43;
    if (!big && u16(head.data() + 2) != 42) return false;
    if (big && (end < 16 || !read_at(in, 8, head.data() + 8, 8))) return false;
    const std::size_t count_size = big ? 8 : 2;
    const std::size_t entry_size = big ? 20 : 12;
    const std::size_t next_size = big ? 8 : 4;

    std::uint64_t ifd = big ? u64(head.data() + 8) : u32(head.data() + 4);
    std::unordered_set<std::uint64_t> seen;
    std::vector<uint8_t> entries;
    while (ifd != 0 && ifd < end && seen.insert(ifd).second) {
        if (end - ifd < count_size || !read_at(in, ifd, head.data(), count_size)) return false;
        const std::uint64_t count = big ? u64(head.data()) : u16(head.data());
        const std::uint64_t first = ifd + count_size;
        if (count > (end - first) / entry_size) return false;
        entries.resize(count * entry_size + next_size);
        if (!read_at(in, first, entries.data(), count * entry_size)) return false;
        for (std::uint64_t i = 0; i < count; ++i) {
            if (u16(entries.data() + i * entry_size) == kManifestTag) return true;
        }
        const std::uint64_t next = first + count * entry_size;
        if (end - next < next_size || !read_at(in, next, entries.data(), next_size)) return false;
        ifd = big ? u64(entries.data()) : u32(entries.data());
    }
    return false;
}

bool jxl_has_manifest(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    const auto end = stream_size(in);
    auto box = read_box(in, 0, end);
    if (!box || box->type() != "JXL ") return false;
    std::array<uint8_t, 24> head{};
    for (box = read_box(in, box->pos + box->size, end); box; box = read_box(in, box->pos + box->size, end)) {
        const auto content = box->pos + box->header;
        if (box->type() == "jumb" && box->size >= box->header + head.size() &&
            read_at(in, content, head.data(), head.size()) && std::memcmp(head.data() + 4, "jumd", 4) == 0 &&
            std::equal(kStoreType.begin(), kStoreType.end(), head.begin() + 8)) {
            return true;
        }
        // metadata boxes may be brotli-compressed behind their original type
        if (box->type() == "brob" && box->size >= box->header + 4 && read_at(in, content, head.data(), 4) &&
            std::memcmp(head.data(), "jumb", 4) == 0) {
            return true;
        }
    }
    return false;
}

bool id3_has_manifest(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::array<uint8_t, 10> head{};
    if (!in.read(reinterpret_cast<char*>(head.data()), head.size()) || std::memcmp(head.data(), "ID3", 3) != 0) {
        return false;
    }
    // the manifest store is a GEOB frame typed by its mime, inside the tag that follows the header
    const std::uint64_t tag_size = static_cast<std::uint64_t>(head[6] & 0x7F) << 21 | (head[7] & 0x7F) << 14 |
                                   (head[8] & 0x7F) << 7 | (head[9] & 0x7F);
    return file_contains(path, {"application/c2pa", "application/x-c2pa-manifest-store"}, head.size() + tag_size);
}

bool gif_has_manifest(const fs::path& path) {
    return file_contains(path, {std::string_view("\x21\xFF\x0B" "C2PA_GIF")});
}

} // namespace chisel::c2pa
