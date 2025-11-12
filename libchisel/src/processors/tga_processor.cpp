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

} // namespace chisel