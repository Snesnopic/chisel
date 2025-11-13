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

namespace fs = std::filesystem;

namespace { // anonymous namespace for helpers

    // --- helpers for raw_equal (from png_processor) ---

    void png_error_fn(png_structp, const png_const_charp msg) {
        Logger::log(LogLevel::Error, std::string("libpng: ") + msg, "libpng");
        throw std::runtime_error(msg);
    }

    void png_warning_fn(png_structp, const png_const_charp msg) {
        Logger::log(LogLevel::Warning, std::string("libpng: ") + msg, "libpng");
    }

    struct FileCloser {
        void operator()(FILE *f) const { if (f) std::fclose(f); }
    };

    using unique_FILE = std::unique_ptr<FILE, FileCloser>;

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
        unique_FILE fp(std::fopen(file.string().c_str(), "rb"));
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

namespace chisel {

void ZopfliPngProcessor::recompress(const fs::path& input,
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
            // keep common metadata chunks
            opts.keepchunks = {"tEXt", "zTXt", "iTXt", "eXIf", "iCCP", "sRGB", "gAMA", "cHRM", "sBIT", "pHYs"};
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

std::vector<unsigned char> ZopfliPngProcessor::recompress_with_zopfli(const std::vector<unsigned char>& input) {
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
        Logger::log(LogLevel::Warning, std::string("raw_equal: Failed to decode PNG (A): ") + a.string() + " (" + e.what() + ")", "zopflipng_processor");
        return false;
    }

    try {
        imgB = decode_png_rgba8(b, wb, hb);
    } catch (const std::exception& e) {
        Logger::log(LogLevel::Warning, std::string("raw_equal: Failed to decode PNG (B): ") + b.string() + " (" + e.what() + ")", "zopflipng_processor");
        return false;
    }

    if (wa != wb || ha != hb) {
        Logger::log(LogLevel::Debug, "raw_equal: dimension mismatch", "zopflipng_processor");
        return false;
    }

    if (imgA != imgB) {
        Logger::log(LogLevel::Debug, "raw_equal: pixel data mismatch", "zopflipng_processor");
        return false;
    }

    return true;
}

} // namespace chisel