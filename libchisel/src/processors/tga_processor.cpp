//
// Created by Giuseppe Francione on 12/11/25.
//

#include "../../include/tga_processor.hpp"
#include "../../include/logger.hpp"
#include <stdexcept>

// --- STB Implementation ---
// define implementations in this single .cpp file
#define STB_IMAGE_IMPLEMENTATION
#include "../../third_party/stb/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../../third_party/stb/stb_image_write.h"
// --------------------------


namespace chisel {

    static const char* processor_tag() {
        return "TgaProcessor";
    }

    void TgaProcessor::recompress(const std::filesystem::path& input,
                                   const std::filesystem::path& output,
                                   bool /*preserve_metadata*/) {

        Logger::log(LogLevel::Info, "Recompressing TGA with RLE: " + input.string(), processor_tag());

        int width, height, channels;
        // load the image
        unsigned char* data = stbi_load(input.string().c_str(), &width, &height, &channels, 0);

        if (!data) {
            Logger::log(LogLevel::Error, std::string("Failed to load TGA: ") + stbi_failure_reason(), processor_tag());
            throw std::runtime_error("TgaProcessor: Failed to load TGA");
        }

        // enable rle compression
        stbi_write_tga_with_rle = 1;

        // write the image back out
        int success = stbi_write_tga(output.string().c_str(), width, height, channels, data);

        // free the image data
        stbi_image_free(data);

        if (!success) {
            Logger::log(LogLevel::Error, "Failed to write RLE TGA: " + output.string(), processor_tag());
            throw std::runtime_error("TgaProcessor: Failed to write TGA");
        }

        Logger::log(LogLevel::Debug, "TGA RLE recompression complete: " + output.string(), processor_tag());
    }

    std::string TgaProcessor::get_raw_checksum(const std::filesystem::path& /*file_path*/) const {
        // not implemented for tga
        return "";
    }
    // helper to load a tga image into a raw rgba8 buffer
static std::vector<unsigned char> decode_tga_rgba8(const std::filesystem::path& file,
                                                   int& width,
                                                   int& height,
                                                   int& channels) {
    // force 4 channels (rgba) for consistent comparison
    unsigned char* data = stbi_load(file.string().c_str(), &width, &height, &channels, 4);
    if (!data) {
        Logger::log(LogLevel::Warning, std::string("raw_equal: Failed to load TGA: ") + stbi_failure_reason(), processor_tag());
        return {};
    }

    channels = 4;
    const size_t data_size = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;

    // copy data to vector for safe handling
    std::vector<unsigned char> pcm(data, data + data_size);

    // free stb memory
    stbi_image_free(data);

    return pcm;
}

bool TgaProcessor::raw_equal(const std::filesystem::path &a,
                             const std::filesystem::path &b) const {
    int wa, ha, ca;
    int wb, hb, cb;

    std::vector<unsigned char> pcmA;
    std::vector<unsigned char> pcmB;

    try {
        pcmA = decode_tga_rgba8(a, wa, ha, ca);
    } catch (const std::exception& e) {
        Logger::log(LogLevel::Error, "raw_equal: Error decoding " + a.string() + ": " + e.what(), processor_tag());
        return false;
    }

    try {
        pcmB = decode_tga_rgba8(b, wb, hb, cb);
    } catch (const std::exception& e) {
        Logger::log(LogLevel::Error, "raw_equal: Error decoding " + b.string() + ": " + e.what(), processor_tag());
        return false;
    }

    // check for load failure
    if (pcmA.empty() || pcmB.empty()) {
        // error already logged by decode_tga_rgba8
        return false;
    }

    // compare dimensions
    if (wa != wb || ha != hb || ca != cb) {
        Logger::log(LogLevel::Debug, "raw_equal: TGA dimension mismatch", processor_tag());
        return false;
    }

    // compare raw pixel data
    if (pcmA != pcmB) {
        Logger::log(LogLevel::Debug, "raw_equal: TGA pixel data mismatch", processor_tag());
        return false;
    }

    return true;
}
} // namespace chisel