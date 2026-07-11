//
// Created by Giuseppe Francione on 20/11/25.
//

#include "../../include/pnm_processor.hpp"
#include "../../include/logger.hpp"
#include "../../include/file_utils.hpp"
#include "../../third_party/stb/stb_image.h"
#include <stdexcept>
#include <vector>
#include <fstream>
#include <cstring> // for memcmp
#include <cctype>
#include <optional>


namespace chisel {

namespace {

struct PnmImage {
    int width = 0;
    int height = 0;
    int channels = 0; // 1 or 3
    std::vector<uint8_t> pixels; // 8-bit, `channels` interleaved
};

void pnm_skip_ws_and_comments(std::istream& is) {
    int c;
    while ((c = is.peek()) != EOF) {
        if (std::isspace(c)) { is.get(); continue; }
        if (c == '#') { std::string line; std::getline(is, line); continue; }
        break;
    }
}

int pnm_read_uint(std::istream& is) {
    pnm_skip_ws_and_comments(is);
    int val = 0;
    bool any = false;
    int c;
    while ((c = is.peek()) != EOF && std::isdigit(c)) {
        val = val * 10 + (is.get() - '0');
        any = true;
    }
    if (!any) throw std::runtime_error("PnmProcessor: malformed header (expected integer)");
    return val;
}

// stb_image only recognizes binary P5/P6, so P1-P4 need to be parsed by hand here
std::optional<PnmImage> decode_ascii_or_bitmap_pnm(const std::filesystem::path& path) {
    std::ifstream is(path, std::ios::binary);
    if (!is) return std::nullopt;

    char p, digit;
    is.get(p);
    is.get(digit);
    if (p != 'P') return std::nullopt;

    PnmImage img;
    switch (digit) {
        case '1': case '4': img.channels = 1; break; // bitmap -> stored as 8-bit grayscale
        case '2': img.channels = 1; break;
        case '3': img.channels = 3; break;
        default: return std::nullopt; // P5/P6 handled by stb_image, anything else unsupported
    }

    img.width = pnm_read_uint(is);
    img.height = pnm_read_uint(is);
    if (img.width <= 0 || img.height <= 0) throw std::runtime_error("PnmProcessor: invalid dimensions");

    const size_t pixel_count = static_cast<size_t>(img.width) * img.height;
    img.pixels.resize(pixel_count * img.channels);

    if (digit == '1') {
        // ASCII bitmap: whitespace-separated 0/1 tokens, 0=white(255) 1=black(0)
        for (size_t i = 0; i < pixel_count; ++i) {
            const int bit = pnm_read_uint(is);
            img.pixels[i] = (bit == 0) ? 255 : 0;
        }
    } else if (digit == '4') {
        // binary bitmap: no maxval field, packed bits MSB-first, each row byte-padded
        pnm_skip_ws_and_comments(is);
        const int row_bytes = (img.width + 7) / 8;
        std::vector<uint8_t> row(row_bytes);
        for (int y = 0; y < img.height; ++y) {
            is.read(reinterpret_cast<char*>(row.data()), row_bytes);
            if (!is) throw std::runtime_error("PnmProcessor: truncated P4 data");
            for (int x = 0; x < img.width; ++x) {
                const int bit = (row[x / 8] >> (7 - (x % 8))) & 1;
                img.pixels[static_cast<size_t>(y) * img.width + x] = bit ? 0 : 255;
            }
        }
    } else {
        // P2/P3: maxval, then whitespace-separated decimal samples, linearly scaled to 0-255
        const int maxval = pnm_read_uint(is);
        if (maxval <= 0) throw std::runtime_error("PnmProcessor: invalid maxval");
        for (size_t i = 0; i < img.pixels.size(); ++i) {
            const int sample = pnm_read_uint(is);
            img.pixels[i] = (maxval == 255) ? static_cast<uint8_t>(sample)
                                             : static_cast<uint8_t>((sample * 255 + maxval / 2) / maxval);
        }
    }

    return img;
}

} // namespace

void PnmProcessor::recompress(const std::filesystem::path& input,
                              const std::filesystem::path& output, const ProcessingOptions &options) {
    Logger::log(LogLevel::Debug, "Entering recompress for " + input.string(), get_name());

    int width, height, channels, desired_channels;
    std::vector<uint8_t> owned_pixels;
    unsigned char* data = nullptr;

    // force loading as grey (1) or rgb (3); alpha not supported in standard pnm
    if (stbi_info(input.string().c_str(), &width, &height, &channels)) {
        desired_channels = (channels == 1) ? 1 : 3;

        FILE* f_in = chisel::open_file(input, "rb");
        if (!f_in) throw std::runtime_error("PnmProcessor: cannot open input");
        data = stbi_load_from_file(f_in, &width, &height, &channels, desired_channels);
        fclose(f_in);

        if (!data) {
            Logger::log(LogLevel::Error, "Failed to load pnm data", get_name());
            throw std::runtime_error("PnmProcessor: decode failed");
        }
    } else {
        // P1-P4: not recognized by stb_image at all, parse by hand
        auto img = decode_ascii_or_bitmap_pnm(input);
        if (!img) {
            Logger::log(LogLevel::Error, "Failed to parse pnm header", get_name());
            throw std::runtime_error("PnmProcessor: invalid input");
        }
        width = img->width;
        height = img->height;
        desired_channels = img->channels;
        owned_pixels = std::move(img->pixels);
        data = owned_pixels.data();
    }

    FILE* f_out = chisel::open_file(output, "wb");
    if (!f_out) {
        if (owned_pixels.empty()) stbi_image_free(data);
        throw std::runtime_error("PnmProcessor: cannot open output");
    }

    // write header: p5 (grey binary) or p6 (rgb binary)
    const char* magic = (desired_channels == 1) ? "P5" : "P6";
    if (fprintf(f_out, "%s\n%d %d\n255\n", magic, width, height) < 0) {
        if (owned_pixels.empty()) stbi_image_free(data);
        fclose(f_out);
        throw std::runtime_error("PnmProcessor: write header failed");
    }

    // write raw binary data
    const size_t data_size = static_cast<std::size_t>(width) * height * desired_channels;
    if (fwrite(data, 1, data_size, f_out) != data_size) {
        if (owned_pixels.empty()) stbi_image_free(data);
        fclose(f_out);
        throw std::runtime_error("PnmProcessor: write data failed");
    }

    fclose(f_out);
    if (owned_pixels.empty()) stbi_image_free(data);

    Logger::log(LogLevel::Debug, "Exiting recompress for " + output.string(), get_name());
}

std::string PnmProcessor::get_raw_checksum(const std::filesystem::path& /*file_path*/) const {
    return "";
}

bool PnmProcessor::raw_equal(const std::filesystem::path& a,
                             const std::filesystem::path& b) const {
    // helper to load data via stb_image (P5/P6) or the manual parser (P1-P4)
    auto load = [](const std::filesystem::path& p, int& w, int& h, int& c) -> std::vector<uint8_t> {
        FILE* f = chisel::open_file(p, "rb");
        if (f) {
            unsigned char* d = stbi_load_from_file(f, &w, &h, &c, 0);
            fclose(f);
            if (d) {
                std::vector<uint8_t> out(d, d + static_cast<size_t>(w) * h * c);
                stbi_image_free(d);
                return out;
            }
        }
        auto img = decode_ascii_or_bitmap_pnm(p);
        if (!img) return {};
        w = img->width; h = img->height; c = img->channels;
        return std::move(img->pixels);
    };

    int w1, h1, c1, w2, h2, c2;
    const auto d1 = load(a, w1, h1, c1);
    const auto d2 = load(b, w2, h2, c2);

    if (d1.empty() || d2.empty()) return false;
    return w1 == w2 && h1 == h2 && c1 == c2 && d1 == d2;
}

} // namespace chisel
