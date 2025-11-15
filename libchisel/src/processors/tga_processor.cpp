//
// Created by Giuseppe Francione on 12/11/25.
//

#include "../../include/tga_processor.hpp"
#include "../../include/logger.hpp"
#include <stdexcept>
#include <memory>

// --- STB Implementation ---
// define implementations in this single .cpp file
#define STB_IMAGE_IMPLEMENTATION
#include "../../third_party/stb/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "file_utils.hpp"
#include "../../third_party/stb/stb_image_write.h"
// --------------------------

namespace {
    struct FileCloser {
        void operator()(FILE* f) const { if (f) std::fclose(f); }
    };

    using unique_FILE = std::unique_ptr<FILE, FileCloser>;

    void stbi_write_callback(void *context, void *data, int size) {
        if (size <= 0) return;
        FILE* f = static_cast<FILE *>(context);
        std::fwrite(data, 1, static_cast<size_t>(size), f);
    }
} // namespace

namespace chisel {

    static const char* processor_tag() {
        return "TgaProcessor";
    }

    void TgaProcessor::recompress(const std::filesystem::path& input,
                                   const std::filesystem::path& output,
                                   bool /*preserve_metadata*/) {

        Logger::log(LogLevel::Info, "Recompressing TGA with RLE: " + input.string(), processor_tag());

        int width, height, channels;
        unique_FILE in_file(chisel::open_file(input, "rb"));
        if (!in_file) {
            Logger::log(LogLevel::Error, "Failed to open input file", processor_tag());
            throw std::runtime_error("TgaProcessor: Cannot open input");
        }
        // load the image
        unsigned char* data = stbi_load_from_file(in_file.get(), &width, &height, &channels, 0);
        in_file.reset();
        if (!data) {
            Logger::log(LogLevel::Error, std::string("Failed to load TGA: ") + stbi_failure_reason(), processor_tag());
            throw std::runtime_error("TgaProcessor: Failed to load TGA");
        }

        unique_FILE out_file(chisel::open_file(output, "wb"));
        if (!out_file) {
            stbi_image_free(data);
            Logger::log(LogLevel::Error, "Failed to open output file", processor_tag());
            throw std::runtime_error("TgaProcessor: Cannot open output");
        }

        // enable rle compression
        stbi_write_tga_with_rle = 1;

        const int success = stbi_write_tga_to_func(
            stbi_write_callback,
            out_file.get(),
            width, height, channels, data
        );

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
    const unique_FILE in_file(chisel::open_file(file, "rb"));
    if (!in_file) {
        Logger::log(LogLevel::Warning, "raw_equal: Failed to open TGA: " + file.string(), processor_tag());
        return {};
    }
    unsigned char* data = stbi_load_from_file(in_file.get(), &width, &height, &channels, 4);
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