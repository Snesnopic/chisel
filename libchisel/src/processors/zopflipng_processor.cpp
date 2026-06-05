//
// Created by Giuseppe Francione on 19/10/25.
//

#include "../../include/zopflipng_processor.hpp"
#include "../../include/logger.hpp"
#include "zopflipng_lib.h"
#include "zlib_container.h"
#include <filesystem>
#include <fstream>
#include <vector>
#include <string>
#include <stdexcept>
#include <png.h>
#include <zlib.h>
#include <cstring>
#include <memory>
#include <iostream>
#include "file_utils.hpp"


namespace chisel {

namespace fs = std::filesystem;

namespace {

    // wrapper for libpng structures (destroys in case of exceptions)
    struct PngRead {
        png_structp png = nullptr;
        png_infop info = nullptr;

        explicit PngRead() = default;

        ~PngRead() {
            if (png || info) png_destroy_read_struct(&png, &info, nullptr);
        }
    };

    std::vector<unsigned char> decode_png_rgba8(const std::filesystem::path &file,
                                                png_uint_32 &width,
                                                png_uint_32 &height) {
        const unique_FILE fp(chisel::open_file(file.string().c_str(), "rb"));
        if (!fp) throw std::runtime_error("Cannot open PNG: " + file.string());

        PngRead rd;
        rd.png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
        if (!rd.png) {
            throw std::runtime_error("png_create_read_struct failed");
        }

        rd.info = png_create_info_struct(rd.png);
        if (!rd.info) {
            throw std::runtime_error("png_create_info_struct failed");
        }

        if (setjmp(png_jmpbuf(rd.png))) {
            throw std::runtime_error("libpng error while reading " + file.string());
        }

        png_init_io(rd.png, fp.get());
        png_read_info(rd.png, rd.info);

        int bit_depth, color_type;
        png_get_IHDR(rd.png, rd.info, &width, &height, &bit_depth, &color_type, nullptr, nullptr, nullptr);

        // configure transforms for consistent rgba8 output
        if (bit_depth == 16) png_set_strip_16(rd.png);
        if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(rd.png);
        if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) png_set_expand_gray_1_2_4_to_8(rd.png);
        if (png_get_valid(rd.png, rd.info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(rd.png);
        if (!(color_type & PNG_COLOR_MASK_ALPHA)) png_set_filler(rd.png, 0xFF, PNG_FILLER_AFTER);
        if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(rd.png);

        png_read_update_info(rd.png, rd.info);

        const size_t rowbytes = png_get_rowbytes(rd.png, rd.info);
        if (rowbytes != static_cast<size_t>(width) * 4) {
             throw std::runtime_error("Rowbytes mismatch, expected RGBA8");
        }

        std::vector<unsigned char> image(rowbytes * height);
        std::vector<png_bytep> row_pointers(height);
        for (png_uint_32 y = 0; y < height; ++y) {
            row_pointers[y] = image.data() + y * rowbytes;
        }

        png_read_image(rd.png, row_pointers.data());
        png_read_end(rd.png, rd.info);

        return image;
    }

} // namespace

void ZopfliPngProcessor::recompress(const fs::path& input,
                                    const fs::path& output, const ProcessingOptions &options) {
    Logger::log(LogLevel::Debug, "Entering recompress for " + input.string(), get_name());

    try {
        // configure options
        ZopfliPNGOptions opts;
        opts.lossy_transparent = false;
        opts.lossy_8bit = false;
        opts.use_zopfli = true;
        opts.num_iterations = options.iterations;
        opts.num_iterations_large = options.iterations_large;

        if (options.preserve_metadata) {
            // keep common metadata and specialized chunks (APNG, 9Patch)
            opts.keepchunks = {"tEXt", "zTXt", "iTXt", "eXIf", "iCCP", "sRGB", "gAMA", "cHRM", "sBIT", "pHYs", "acTL", "fcTL", "fdAT", "npTc"};
        } else {
            // even if not preserving metadata, we MUST keep animation/scaling chunks to avoid breaking the file functionality
            opts.keepchunks = {"acTL", "fcTL", "fdAT", "npTc"};
        }

        std::vector<unsigned char> origpng;
        try {
            origpng = chisel::read_file(input);
        } catch (const std::exception& e) {
            Logger::log(LogLevel::Error, std::string("Failed to open input file: ") + e.what(), get_name());
            throw std::runtime_error("ZopflipngProcessor: cannot open input");
        }

        // optimize
        std::vector<unsigned char> resultpng;
        if (ZopfliPNGOptimize(origpng, opts, false, &resultpng) != 0) {
            Logger::log(LogLevel::Error, "Zopflipng optimization failed for: " + input.string(), get_name());
            throw std::runtime_error("ZopflipngProcessor: optimization failed");
        }

        // write output file
        std::ofstream ofs(output, std::ios::binary);
        ofs.write(reinterpret_cast<const char*>(resultpng.data()), resultpng.size());

        Logger::log(LogLevel::Debug, "Exiting recompress for " + output.string(), get_name());
    }
    catch (const std::exception& e) {
        Logger::log(LogLevel::Error, std::string("Exception during zopflipng optimization: ") + e.what(), get_name());
        throw;
    }
}

std::string ZopfliPngProcessor::get_raw_checksum(const std::filesystem::path&) const {
    // TODO: implement checksum of raw PNG data
    return "";
}

bool ZopfliPngProcessor::raw_equal(const std::filesystem::path &a,
                                 const std::filesystem::path &b) const {
    png_uint_32 wa, ha, wb, hb;
    std::vector<unsigned char> imgA, imgB;

    try {
        imgA = decode_png_rgba8(a, wa, ha);
    } catch (const std::exception& e) {
        Logger::log(LogLevel::Warning, std::string("Raw_equal: Failed to decode png (a): ") + a.string() + " (" + e.what() + ")", get_name());
        return false;
    }

    try {
        imgB = decode_png_rgba8(b, wb, hb);
    } catch (const std::exception& e) {
        Logger::log(LogLevel::Warning, std::string("Raw_equal: Failed to decode png (b): ") + b.string() + " (" + e.what() + ")", get_name());
        return false;
    }

    if (wa != wb || ha != hb) {
        Logger::log(LogLevel::Debug, "Raw_equal: dimension mismatch", get_name());
        return false;
    }

    if (imgA != imgB) {
        Logger::log(LogLevel::Debug, "Raw_equal: pixel data mismatch", get_name());
        return false;
    }

    return true;
}

} // namespace chisel
