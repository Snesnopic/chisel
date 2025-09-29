//
// Created by Giuseppe Francione on 28/09/25.
//

#include "../utils/logger.hpp"
#include "zopflipng_encoder.hpp"
#include "zopflipng_lib.h"
#include <filesystem>
#include <fstream>
#include <vector>
#include <string>
#include <stdexcept>

namespace fs = std::filesystem;

ZopfliPngEncoder::ZopfliPngEncoder(const bool preserve_metadata) {
    preserve_metadata_ = preserve_metadata;
}

bool ZopfliPngEncoder::recompress(const fs::path& input,
                                  const fs::path& output)
{
    Logger::log(LogLevel::INFO, "Starting PNG optimization with ZopfliPNG: " + input.string(), "ZopfliPngEncoder");

    try {
        // configure options
        ZopfliPNGOptions opts;
        opts.lossy_transparent = false;
        opts.lossy_8bit = false;
        opts.num_iterations = 15;
        opts.num_iterations_large = 5;
        if (preserve_metadata_) {
            opts.keepchunks = {"tEXt", "zTXt", "iTXt", "exif"};
        } else {
            opts.keepchunks.clear();
        }

        // read input file
        std::ifstream ifs(input, std::ios::binary);
        if (!ifs) {
            Logger::log(LogLevel::ERROR, "Failed to open input file", "ZopfliPngEncoder");
            return false;
        }
        const std::vector<unsigned char> origpng((std::istreambuf_iterator<char>(ifs)),
                                           std::istreambuf_iterator<char>());

        // optimize
        std::vector<unsigned char> resultpng;

        if (ZopfliPNGOptimize(origpng, opts, false, &resultpng) != 0) {
            Logger::log(LogLevel::ERROR, "ZopfliPNG optimization failed for: " + input.string(), "ZopfliPngEncoder");
            return false;
        }

        // write output file
        std::ofstream ofs(output, std::ios::binary);
        ofs.write(reinterpret_cast<const char*>(resultpng.data()), resultpng.size());

        Logger::log(LogLevel::INFO, "PNG optimization finished: " + output.string(), "ZopfliPngEncoder");
        return true;
    }
    catch (const std::exception& e) {
        Logger::log(LogLevel::ERROR, std::string("Exception during ZopfliPNG optimization: ") + e.what(), "ZopfliPngEncoder");
        return false;
    }
}