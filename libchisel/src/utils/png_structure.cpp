//
// Created by Giuseppe Francione on 24/09/26.
//

#include "../../include/png_structure.hpp"
#include "../../include/file_utils.hpp"
#include <algorithm>
#include <cstdint>
#include <string_view>

namespace chisel::png {

namespace {

constexpr std::array<unsigned char, 8> kSignature = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

struct ChunkRef {
    std::size_t offset = 0; // of the length field
    std::size_t size = 0;   // length, type, data and crc
    std::string_view type;
};

// every chunk up to IEND included, or nothing for a malformed stream
std::optional<std::vector<ChunkRef>> chunk_list(const std::span<const unsigned char> data) {
    if (data.size() < kSignature.size() || !std::equal(kSignature.begin(), kSignature.end(), data.begin())) {
        return std::nullopt;
    }
    std::vector<ChunkRef> chunks;
    for (std::size_t pos = kSignature.size();;) {
        if (data.size() - pos < 12) return std::nullopt;
        const std::uint32_t length = read_be32(data.data() + pos);
        if (length > data.size() - pos - 12) return std::nullopt;
        chunks.push_back({.offset=pos, .size=std::size_t{length} + 12,
                          .type=std::string_view(reinterpret_cast<const char*>(data.data() + pos + 4), 4)});
        pos += chunks.back().size;
        if (chunks.back().type == "IEND") return chunks;
    }
}

enum class Carry { Keep, Drop, ColorBound, Block };

Carry classify(const std::string_view type, const bool preserve_metadata, const bool keep_animation) {
    using namespace std::string_view_literals;
    // encoders write these anew
    for (const auto rewritten : {"IHDR"sv, "PLTE"sv, "IDAT"sv, "IEND"sv, "tRNS"sv}) {
        if (type == rewritten) return Carry::Drop;
    }
    if (type == "acTL" || type == "fcTL" || type == "fdAT") return keep_animation ? Carry::Keep : Carry::Drop;
    // how the image looks or is used, kept without metadata too
    for (const auto kept : {"iCCP"sv, "sRGB"sv, "gAMA"sv, "cHRM"sv, "cICP"sv, "mDCV"sv, "cLLI"sv, "sTER"sv,
                            "npTc"sv, "npLb"sv, "npOl"sv}) {
        if (type == kept) return Carry::Keep;
    }
    if (!preserve_metadata) return Carry::Drop;
    // values given in terms of the source's color type, bit depth or palette
    for (const auto bound : {"bKGD"sv, "sBIT"sv, "hIST"sv, "pCAL"sv}) {
        if (type == bound) return Carry::ColorBound;
    }
    // apple's index of offsets into the image data, stale once that's rewritten
    if (type == "iDOT") return Carry::Drop;
    for (const auto known : {"tEXt"sv, "zTXt"sv, "iTXt"sv, "eXIf"sv, "tIME"sv, "pHYs"sv, "oFFs"sv, "sCAL"sv,
                             "sPLT"sv, "gIFg"sv, "gIFx"sv, "dSIG"sv, "caBX"sv}) {
        if (type == known) return Carry::Keep;
    }
    // a lowercase last letter marks chunks that stay valid whatever happens to the image data
    return (static_cast<unsigned char>(type[3]) & 0x20) != 0 ? Carry::Keep : Carry::Block;
}

} // namespace

std::optional<std::size_t> image_end(const std::span<const unsigned char> data) {
    const auto chunks = chunk_list(data);
    if (!chunks) return std::nullopt;
    return chunks->back().offset + chunks->back().size;
}

std::optional<std::vector<unsigned char>> trailing_data(const std::filesystem::path& file) {
    std::vector<unsigned char> data;
    if (!read_file(file, data)) return std::nullopt;
    const auto end = image_end(data);
    if (!end) return std::nullopt;
    return std::vector<unsigned char>(data.begin() + static_cast<std::ptrdiff_t>(*end), data.end());
}

std::optional<CarriedChunks> carried_chunks(const std::vector<unsigned char>& png, const bool preserve_metadata,
                                            const bool keep_animation) {
    const auto chunks = chunk_list(png);
    if (!chunks) return std::nullopt;
    CarriedChunks carried;
    std::size_t group = 0;
    for (const auto& chunk : *chunks) {
        if (chunk.type == "PLTE") group = std::max<std::size_t>(group, 1);
        if (chunk.type == "IDAT") group = 2;
        switch (classify(chunk.type, preserve_metadata, keep_animation)) {
            case Carry::Drop:
                break;
            case Carry::Block:
                carried.blocked = true;
                break;
            case Carry::ColorBound:
                carried.color_bound = true;
                [[fallthrough]];
            case Carry::Keep:
                carried.groups[group].emplace_back(png.begin() + static_cast<std::ptrdiff_t>(chunk.offset),
                                                   png.begin() + static_cast<std::ptrdiff_t>(chunk.offset + chunk.size));
                break;
        }
    }
    return carried;
}

bool insert_chunks(std::vector<unsigned char>& png, const CarriedChunks& chunks) {
    const auto layout = chunk_list(png);
    if (!layout) return false;
    std::optional<std::size_t> palette, image, end;
    for (const auto& chunk : *layout) {
        if (chunk.type == "PLTE" && !palette) palette = chunk.offset;
        if ((chunk.type == "IDAT" || chunk.type == "fcTL") && !image) image = chunk.offset;
        if (chunk.type == "IEND") end = chunk.offset;
    }
    if (!image || !end) return false;
    const std::array<std::size_t, 3> at = {std::min(palette.value_or(*image), *image), *image, *end};
    // from the back, so the earlier offsets stay valid
    for (std::size_t g = at.size(); g-- > 0;) {
        std::vector<unsigned char> block;
        for (const auto& chunk : chunks.groups[g]) block.insert(block.end(), chunk.begin(), chunk.end());
        png.insert(png.begin() + static_cast<std::ptrdiff_t>(at[g]), block.begin(), block.end());
    }
    return true;
}

} // namespace chisel::png
