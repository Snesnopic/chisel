//
// Created by Giuseppe Francione on 24/09/26.
//

#include "../../include/png_structure.hpp"
#include "../../include/file_utils.hpp"
#include <png.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
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

namespace {

// owned through a pointer taken before setjmp, so a longjmp leaves it intact
struct ImageScan {
    std::span<const unsigned char> data;
    std::size_t pos = 0;
    std::string error;
    std::vector<unsigned char> row;
    std::vector<png_bytep> rows;
};

void image_scan_read(png_structp png, png_bytep out, const png_size_t size) {
    auto* scan = static_cast<ImageScan*>(png_get_io_ptr(png));
    if (size > scan->data.size() - scan->pos) png_error(png, "truncated");
    std::memcpy(out, scan->data.data() + scan->pos, size);
    scan->pos += size;
}

[[noreturn]] void image_scan_error(png_structp png, png_const_charp msg) {
    static_cast<ImageScan*>(png_get_error_ptr(png))->error = msg ? msg : "unknown error";
    png_longjmp(png, 1);
}

void image_scan_warning(png_structp, png_const_charp) {}

} // namespace

ImageCheck check_image(const std::span<const unsigned char> data) {
    ImageCheck result;
    const auto scan = std::make_unique<ImageScan>();
    scan->data = data;
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, scan.get(), image_scan_error, image_scan_warning);
    if (!png) {
        result.error = "out of memory";
        return result;
    }
    png_infop info = png_create_info_struct(png);
    struct Destroy {
        png_structp& png;
        png_infop& info;
        ~Destroy() { png_destroy_read_struct(&png, info ? &info : nullptr, nullptr); }
    } destroy{png, info};
    if (!info) {
        result.error = "out of memory";
        return result;
    }
    if (setjmp(png_jmpbuf(png))) {
        result.error = scan->error;
        return result;
    }

    png_set_read_fn(png, scan.get(), image_scan_read);
    png_set_benign_errors(png, 1);
    png_set_crc_action(png, PNG_CRC_ERROR_QUIT, PNG_CRC_ERROR_QUIT);
    // libpng tracks the largest index itself, as long as no transform expands the palette first
    png_set_check_for_invalid_index(png, 1);
    png_read_info(png, info);
    png_set_interlace_handling(png);
    png_read_update_info(png, info);
    // every row lands in the same buffer: only the reading and the indices matter
    scan->row.resize(png_get_rowbytes(png, info));

    png_uint_32 images = 1;
    bool animated = false;
#ifdef PNG_APNG_SUPPORTED
    if (png_get_valid(png, info, PNG_INFO_acTL)) {
        png_uint_32 frames = 0, plays = 0;
        png_get_acTL(png, info, &frames, &plays);
        animated = true;
        images = frames + (png_get_first_frame_is_hidden(png, info) ? 1 : 0);
    }
#endif
    for (png_uint_32 i = 0; i < images; ++i) {
        png_uint_32 height = png_get_image_height(png, info);
#ifdef PNG_READ_APNG_SUPPORTED
        if (animated) {
            png_read_frame_head(png, info);
            if (png_get_valid(png, info, PNG_INFO_fcTL)) height = png_get_next_frame_height(png, info);
        }
#endif
        scan->rows.assign(height, scan->row.data());
        png_read_image(png, scan->rows.data());
    }
    png_read_end(png, nullptr);

    png_colorp palette = nullptr;
    int palette_size = 0;
    result.readable = true;
    result.palette_out_of_range = png_get_color_type(png, info) == PNG_COLOR_TYPE_PALETTE &&
                                  png_get_PLTE(png, info, &palette, &palette_size) &&
                                  png_get_palette_max(png, info) >= palette_size;
    return result;
}

} // namespace chisel::png
