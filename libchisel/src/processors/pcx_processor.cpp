//
// Created by Giuseppe Francione on 05/06/26.
//

#include "../../include/pcx_processor.hpp"
#include "../../include/logger.hpp"
#include "../../include/file_utils.hpp"
#include <filesystem>
#include <fstream>
#include <vector>
#include <stdexcept>
#include <cstring>
#include <memory>

namespace chisel {

namespace {

#pragma pack(push, 1)
struct PcxHeader {
    uint8_t identifier;      // 0x0A
    uint8_t version;         // 5 = VGA palette
    uint8_t encoding;        // 1 = RLE
    uint8_t bits_per_pixel;  // 1, 2, 4, 8
    uint16_t x_min, y_min, x_max, y_max;
    uint16_t h_res, v_res;
    uint8_t palette[48];     // 16-color EGA palette
    uint8_t reserved;        // 0
    uint8_t color_planes;    // 1, 3, 4
    uint16_t bytes_per_line; // Must be even
    uint16_t palette_type;   // 1 = Color/BW, 2 = Grayscale
    uint16_t screen_width, screen_height;
    uint8_t filler[54];
};
#pragma pack(pop)

/**
 * @brief Canonical PCX RLE encoder mimicking ImageMagick's behavior.
 */
void encode_pcx_rle(const uint8_t* raw_data, size_t length, std::ostream& os) {
    for (size_t i = 0; i < length; ) {
        uint8_t current = raw_data[i];
        uint8_t count = 1;

        // ImageMagick looks for runs up to 63 bytes
        while (count < 63 && (i + count) < length && raw_data[i + count] == current) {
            count++;
        }

        if (count > 1 || current >= 192) {
            os.put(static_cast<char>(0xC0 | count));
            os.put(static_cast<char>(current));
        } else {
            os.put(static_cast<char>(current));
        }
        i += count;
    }
}

/**
 * @brief Decodes a PCX stream into a raw pixel buffer.
 */
std::vector<uint8_t> decode_pcx(std::istream& is, PcxHeader& header, std::vector<uint8_t>& palette) {
    is.read(reinterpret_cast<char*>(&header), sizeof(PcxHeader));
    if (!is || header.identifier != 0x0A) throw std::runtime_error("Invalid PCX identifier");

    const size_t width = static_cast<size_t>(header.x_max) - header.x_min + 1;
    const size_t height = static_cast<size_t>(header.y_max) - header.y_min + 1;
    const size_t bpl = header.bytes_per_line;
    const size_t planes = header.color_planes;

    Logger::log(LogLevel::Debug, "PCX Dimensions: " + std::to_string(width) + "x" + std::to_string(height) + " planes=" + std::to_string(planes) + " bpl=" + std::to_string(bpl), "PcxProcessor");

    if (width == 0 || height == 0 || planes == 0 || bpl == 0 || planes > 4 || width > 32768 || height > 32768) {
        throw std::runtime_error("PcxProcessor: invalid or unsupported image dimensions/planes");
    }

    std::vector<uint8_t> pixels(width * height * planes);
    std::vector<uint8_t> scanline_buffer(bpl * planes);

    for (size_t y = 0; y < height; ++y) {
        // Decompress RLE scanline
        size_t bytes_read = 0;
        while (bytes_read < scanline_buffer.size()) {
            uint8_t b;
            if (!is.read(reinterpret_cast<char*>(&b), 1)) break;
            if ((b & 0xC0) == 0xC0) {
                uint8_t count = b & 0x3F;
                uint8_t val;
                if (!is.read(reinterpret_cast<char*>(&val), 1)) break;
                for (uint8_t i = 0; i < count && bytes_read < scanline_buffer.size(); ++i) {
                    scanline_buffer[bytes_read++] = val;
                }
            } else {
                scanline_buffer[bytes_read++] = b;
            }
        }

        // Interleave planes into RGB/Grayscale buffer
        for (size_t p = 0; p < planes; ++p) {
            for (size_t x = 0; x < width; ++x) {
                pixels[(y * width * planes) + (x * planes) + p] = scanline_buffer[(p * bpl) + x];
            }
        }
    }

    // Read 256-color palette if present
    if (header.version == 5 && header.bits_per_pixel == 8 && header.color_planes == 1) {
        auto current_pos = is.tellg();
        is.seekg(-769, std::ios::end);
        uint8_t marker;
        is.read(reinterpret_cast<char*>(&marker), 1);
        if (marker == 0x0C) {
            palette.resize(768);
            is.read(reinterpret_cast<char*>(palette.data()), 768);
        } else {
            is.seekg(current_pos); // Palette not found at end, restore position
        }
    }

    return pixels;
}

/**
 * @brief Writes a PCX file to a stream using optimized RLE.
 */
void write_pcx_internal(std::ostream& os, const PcxHeader& orig_header, const std::vector<uint8_t>& pixels, const std::vector<uint8_t>& palette, bool preserve_metadata) {
    PcxHeader header = orig_header;
    
    if (!preserve_metadata) {
        // Zero out filler and other potential metadata areas
        std::memset(header.filler, 0, sizeof(header.filler));
        header.h_res = 72; // Normalize resolution
        header.v_res = 72;
    }

    os.write(reinterpret_cast<const char*>(&header), sizeof(header));

    const size_t width = static_cast<size_t>(header.x_max) - header.x_min + 1;
    const size_t height = static_cast<size_t>(header.y_max) - header.y_min + 1;
    const size_t bpl = header.bytes_per_line;
    const size_t planes = header.color_planes;

    Logger::log(LogLevel::Debug, "PCX Dimensions: " + std::to_string(width) + "x" + std::to_string(height) + " planes=" + std::to_string(planes) + " bpl=" + std::to_string(bpl), "PcxProcessor");

    std::vector<uint8_t> scanline_buffer(bpl);

    for (size_t y = 0; y < height; ++y) {
        for (size_t p = 0; p < planes; ++p) {
            std::memset(scanline_buffer.data(), 0, bpl);
            for (size_t x = 0; x < width; ++x) {
                scanline_buffer[x] = pixels[(y * width * planes) + (x * planes) + p];
            }
            encode_pcx_rle(scanline_buffer.data(), bpl, os);
        }
    }

    if (!palette.empty()) {
        os.put(0x0C);
        os.write(reinterpret_cast<const char*>(palette.data()), palette.size());
    }
}

} // namespace

void PcxProcessor::recompress(const std::filesystem::path& input,
                               const std::filesystem::path& output,
                               const ProcessingOptions &options) {
    Logger::log(LogLevel::Debug, "Entering recompress for " + input.string(), get_name());

    // DCX handling (multi-page)
    std::ifstream is(input, std::ios::binary);
    uint8_t id_buf[4];
    is.read(reinterpret_cast<char*>(id_buf), 4);
    uint32_t dcx_id = read_le32(id_buf);

    if (dcx_id == 0x3ADE68B1) { // DCX Multi-page
        Logger::log(LogLevel::Info, "Processing DCX multi-page container", get_name());
        std::vector<uint32_t> offsets;
        for (int i = 0; i < 1024; ++i) {
            uint8_t off_buf[4];
            is.read(reinterpret_cast<char*>(off_buf), 4);
            uint32_t off = read_le32(off_buf);
            if (off == 0) break;
            offsets.push_back(off);
        }

        std::ofstream os(output, std::ios::binary);
        uint8_t sig_buf[4];
        write_le32(sig_buf, 0x3ADE68B1);
        os.write(reinterpret_cast<const char*>(sig_buf), 4);

        std::vector<uint32_t> new_offsets_placeholder(1024, 0);
        for (uint32_t val : new_offsets_placeholder) {
            uint8_t tmp[4];
            write_le32(tmp, val);
            os.write(reinterpret_cast<const char*>(tmp), 4);
        }

        std::vector<uint32_t> new_offsets;
        for (uint32_t off : offsets) {
            new_offsets.push_back(static_cast<uint32_t>(os.tellp()));
            is.seekg(off);
            PcxHeader h;
            std::vector<uint8_t> pal;
            auto pix = decode_pcx(is, h, pal);
            write_pcx_internal(os, h, pix, pal, options.preserve_metadata);
        }

        os.seekp(4);
        for (uint32_t off : new_offsets) {
            uint8_t tmp[4];
            write_le32(tmp, off);
            os.write(reinterpret_cast<const char*>(tmp), 4);
        }

    } else {
        // Single PCX
        is.seekg(0);
        PcxHeader h;
        std::vector<uint8_t> pal;
        auto pix = decode_pcx(is, h, pal);
        
        std::ofstream os(output, std::ios::binary);
        write_pcx_internal(os, h, pix, pal, options.preserve_metadata);
    }

    Logger::log(LogLevel::Debug, "Exiting recompress for " + output.string(), get_name());
}

bool PcxProcessor::raw_equal(const std::filesystem::path& a, const std::filesystem::path& b) const {
    auto get_all_pixels = [](const std::filesystem::path& p) {
        std::vector<std::vector<uint8_t>> all_pixels;
        std::ifstream is(p, std::ios::binary);
        if (!is) return all_pixels;
        
        uint32_t dcx_id = 0;
        is.read(reinterpret_cast<char*>(&dcx_id), 4);
        
        if (dcx_id == 0x3ADE68B1) { // DCX
            std::vector<uint32_t> offsets;
            for (int i = 0; i < 1024; ++i) {
                uint32_t off;
                is.read(reinterpret_cast<char*>(&off), 4);
                if (off == 0) break;
                offsets.push_back(off);
            }
            for (uint32_t off : offsets) {
                is.seekg(off);
                PcxHeader h;
                std::vector<uint8_t> pal;
                try {
                    all_pixels.push_back(decode_pcx(is, h, pal));
                } catch (...) {}
            }
        } else {
            // Single PCX
            is.seekg(0);
            PcxHeader h;
            std::vector<uint8_t> pal;
            try {
                all_pixels.push_back(decode_pcx(is, h, pal));
            } catch (...) {}
        }
        return all_pixels;
    };

    return get_all_pixels(a) == get_all_pixels(b);
}

std::string PcxProcessor::get_raw_checksum(const std::filesystem::path& /*file_path*/) const {
    return "";
}

} // namespace chisel
