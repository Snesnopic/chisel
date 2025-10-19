//
// Created by Giuseppe Francione on 19/10/25.
//

#include "zopflipng_processor.hpp"
#include "zopflipng_lib.h"
#include "zlib_container.h"
#include <filesystem>
#include <fstream>
#include <vector>
#include <string>
#include <stdexcept>

namespace fs = std::filesystem;

namespace chisel {

void ZopflipngProcessor::recompress(const fs::path& input,
                                    const fs::path& output,
                                    bool preserve_metadata) {
    Logger::log(LogLevel::Info, "Starting PNG optimization with ZopfliPNG: " + input.string(), "zopflipng_processor");

    try {
        // configure options
        ZopfliPNGOptions opts;
        opts.lossy_transparent = false;
        opts.lossy_8bit = false;
        opts.use_zopfli = true;
        opts.num_iterations = 15;
        opts.num_iterations_large = 5;

        if (preserve_metadata) {
            opts.keepchunks = {"tEXt", "zTXt", "iTXt", "eXIf"};
        } else {
            opts.keepchunks.clear();
        }

        // read input file
        std::ifstream ifs(input, std::ios::binary);
        if (!ifs) {
            Logger::log(LogLevel::Error, "Failed to open input file", "zopflipng_processor");
            throw std::runtime_error("ZopflipngProcessor: cannot open input");
        }
        auto size = fs::file_size(input);
        std::vector<unsigned char> origpng;
        origpng.reserve(size);
        origpng.assign((std::istreambuf_iterator<char>(ifs)),
                       std::istreambuf_iterator<char>());

        // optimize
        std::vector<unsigned char> resultpng;
        if (ZopfliPNGOptimize(origpng, opts, false, &resultpng) != 0) {
            Logger::log(LogLevel::Error, "ZopfliPNG optimization failed for: " + input.string(), "zopflipng_processor");
            throw std::runtime_error("ZopflipngProcessor: optimization failed");
        }

        // write output file
        std::ofstream ofs(output, std::ios::binary);
        ofs.write(reinterpret_cast<const char*>(resultpng.data()), resultpng.size());

        Logger::log(LogLevel::Info, "PNG optimization finished: " + output.string(), "zopflipng_processor");
    }
    catch (const std::exception& e) {
        Logger::log(LogLevel::Error, std::string("Exception during ZopfliPNG optimization: ") + e.what(), "zopflipng_processor");
        throw;
    }
}

std::vector<unsigned char> ZopflipngProcessor::recompress_with_zopfli(const std::vector<unsigned char>& input) {
    ZopfliOptions opts;
    ZopfliInitOptions(&opts);
    opts.numiterations = 15;
    opts.blocksplitting = 1;

    unsigned char* out_data = nullptr;
    size_t out_size = 0;
    ZopfliZlibCompress(&opts, input.data(), input.size(), &out_data, &out_size);

    std::vector<unsigned char> result(out_data, out_data + out_size);
    free(out_data);
    return result;
}

std::string ZopflipngProcessor::get_raw_checksum(const std::filesystem::path&) const {
    // TODO: implement checksum of raw PNG data
    return "";
}

} // namespace chisel