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

namespace chisel {

static const char* processor_tag() {
    return "PnmProcessor";
}

void PnmProcessor::recompress(const std::filesystem::path& input,
                              const std::filesystem::path& output,
                              bool /*preserve_metadata*/) {
    Logger::log(LogLevel::Info, "Recompressing PNM: " + input.string(), processor_tag());

    int width, height, channels;
    // force loading as grey (1) or rgb (3); alpha not supported in standard pnm
    if (!stbi_info(input.string().c_str(), &width, &height, &channels)) {
         Logger::log(LogLevel::Error, "Failed to parse PNM header", processor_tag());
         throw std::runtime_error("PnmProcessor: invalid input");
    }

    const int desired_channels = (channels == 1) ? 1 : 3;

    FILE* f_in = chisel::open_file(input, "rb");
    if (!f_in) {
        throw std::runtime_error("PnmProcessor: cannot open input");
    }

    unsigned char* data = stbi_load_from_file(f_in, &width, &height, &channels, desired_channels);
    fclose(f_in);

    if (!data) {
        Logger::log(LogLevel::Error, "Failed to load PNM data", processor_tag());
        throw std::runtime_error("PnmProcessor: decode failed");
    }

    FILE* f_out = chisel::open_file(output, "wb");
    if (!f_out) {
        stbi_image_free(data);
        throw std::runtime_error("PnmProcessor: cannot open output");
    }

    // write header: p5 (grey binary) or p6 (rgb binary)
    const char* magic = (desired_channels == 1) ? "P5" : "P6";
    if (fprintf(f_out, "%s\n%d %d\n255\n", magic, width, height) < 0) {
        stbi_image_free(data);
        fclose(f_out);
        throw std::runtime_error("PnmProcessor: write header failed");
    }

    // write raw binary data
    const size_t data_size = static_cast<size_t>(width) * height * desired_channels;
    if (fwrite(data, 1, data_size, f_out) != data_size) {
        stbi_image_free(data);
        fclose(f_out);
        throw std::runtime_error("PnmProcessor: write data failed");
    }

    fclose(f_out);
    stbi_image_free(data);

    Logger::log(LogLevel::Info, "PNM recompression finished: " + output.string(), processor_tag());
}

std::string PnmProcessor::get_raw_checksum(const std::filesystem::path& /*file_path*/) const {
    return "";
}

bool PnmProcessor::raw_equal(const std::filesystem::path& a,
                             const std::filesystem::path& b) const {
    int w1, h1, c1;
    int w2, h2, c2;

    // helper to load data
    auto load = [](const std::filesystem::path& p, int& w, int& h, int& c) -> unsigned char* {
        FILE* f = chisel::open_file(p, "rb");
        if (!f) return nullptr;
        unsigned char* d = stbi_load_from_file(f, &w, &h, &c, 0);
        fclose(f);
        return d;
    };

    unsigned char* d1 = load(a, w1, h1, c1);
    unsigned char* d2 = load(b, w2, h2, c2);

    if (!d1 || !d2) {
        if (d1) stbi_image_free(d1);
        if (d2) stbi_image_free(d2);
        return false;
    }

    bool equal = (w1 == w2) && (h1 == h2) && (c1 == c2);
    if (equal) {
        const size_t sz = static_cast<size_t>(w1) * h1 * c1;
        equal = (std::memcmp(d1, d2, sz) == 0);
    }

    stbi_image_free(d1);
    stbi_image_free(d2);
    return equal;
}

} // namespace chisel