//
// Created by Giuseppe Francione on 20/11/25.
//

#include "../../include/bmp_processor.hpp"
#include "../../include/logger.hpp"
#include "../../include/file_utils.hpp"
#include <stdexcept>
#include <vector>
#include <cstdio>
#include <string>

extern "C" {
#include "bmplib.h"
}

namespace {
    // Helper to convert bmplib result codes to readable strings
    std::string bmplib_result_to_string(const BMPRESULT res) {
        switch (res) {
            case BMP_RESULT_OK:        return "OK";
            case BMP_RESULT_INVALID:   return "Invalid pixel data";
            case BMP_RESULT_TRUNCATED: return "File truncated";
            case BMP_RESULT_INSANE:    return "Image dimensions too large (sanity check failed)";
            case BMP_RESULT_PNG:       return "Embedded PNG (unsupported)";
            case BMP_RESULT_JPEG:      return "Embedded JPEG (unsupported)";
            case BMP_RESULT_ERROR:     return "Generic error";
            case BMP_RESULT_ARRAY:     return "OS/2 Bitmap Array (unsupported)";
            default:                   return "Unknown result code (" + std::to_string(res) + ")";
        }
    }

    // RAII wrapper for bmphandle to ensure free
    struct ScopedBmp {
        BMPHANDLE h = nullptr;
        FILE* f = nullptr;

        ScopedBmp(const std::filesystem::path& path, const char* mode) {
            // Use chisel::open_file for Unicode support on Windows
            f = chisel::open_file(path, mode);
        }

        ~ScopedBmp() {
            if (h) bmp_free(h);
            if (f) fclose(f);
        }

        // Disable copy
        ScopedBmp(const ScopedBmp&) = delete;
        ScopedBmp& operator=(const ScopedBmp&) = delete;
    };
}

namespace chisel {

static const char* processor_tag() {
    return "BmpProcessor";
}

void BmpProcessor::recompress(const std::filesystem::path& input,
                              const std::filesystem::path& output,
                              bool preserve_metadata) {
    Logger::log(LogLevel::Info, "Recompressing BMP: " + input.string(), processor_tag());

    // 1. READ
    ScopedBmp in(input, "rb");
    if (!in.f) {
        Logger::log(LogLevel::Error, "Failed to open input BMP", processor_tag());
        throw std::runtime_error("BmpProcessor: cannot open input");
    }

    in.h = bmpread_new(in.f);
    if (!in.h) throw std::runtime_error("BmpProcessor: failed to create read handle");

    // Load info
    BMPRESULT res = bmpread_load_info(in.h);
    if (res != BMP_RESULT_OK) {
        std::string err = bmp_errmsg(in.h);
        if (err.empty()) err = bmplib_result_to_string(res);

        // If it's an array (BA), extraction should handle it, but recompress can't.
        if (res == BMP_RESULT_ARRAY) {
             Logger::log(LogLevel::Warning, "Input is a Bitmap Array (BA), skipping recompression.", processor_tag());
             // We stop here without throwing to avoid flagging it as a critical error in the batch
             std::error_code ec;
             std::filesystem::copy_file(input, output, std::filesystem::copy_options::overwrite_existing, ec);
             return;
        }

        Logger::log(LogLevel::Error, "Bmplib read error: " + err, processor_tag());
        throw std::runtime_error("BmpProcessor: failed to load info");
    }

    // Get dimensions & properties
    int width, height, channels, bits;
    bmpread_dimensions(in.h, &width, &height, &channels, &bits, nullptr);

    // Check for 64-bit BMP
    const bool is_64bit = bmpread_is_64bit(in.h);

    // Handle Palette
    unsigned char* img_buffer = nullptr;
    unsigned char* palette = nullptr;
    const int num_colors = bmpread_num_palette_colors(in.h);
    const bool is_indexed = (num_colors > 0);

    if (is_indexed) {
        if (bmpread_load_palette(in.h, &palette) != BMP_RESULT_OK) {
             throw std::runtime_error("BmpProcessor: failed to load palette");
        }
        // If palette loaded, bmplib returns 8-bit indices
        channels = 1;
        bits = 8;
    }

    // Handle ICC Profile (Metadata)
    unsigned char* icc_profile = nullptr;
    size_t icc_size = 0;
    if (preserve_metadata) {
        icc_size = bmpread_iccprofile_size(in.h);
        if (icc_size > 0) {
             // buffer is allocated by bmplib
             if (bmpread_load_iccprofile(in.h, &icc_profile) != BMP_RESULT_OK) {
                 Logger::log(LogLevel::Warning, "Failed to load ICC profile, continuing without it.", processor_tag());
                 if (icc_profile) { free(icc_profile); icc_profile = nullptr; }
                 icc_size = 0;
             }
        }
    }

    // Get Resolution (DPI) - Metadata
    int xdpi = 0, ydpi = 0;
    if (preserve_metadata) {
        xdpi = bmpread_resolution_xdpi(in.h);
        ydpi = bmpread_resolution_ydpi(in.h);
    }

    // Allocate image buffer
    const size_t buf_size = bmpread_buffersize(in.h);
    img_buffer = static_cast<unsigned char*>(malloc(buf_size));
    if (!img_buffer) {
        if (palette) free(palette);
        if (icc_profile) free(icc_profile);
        throw std::runtime_error("BmpProcessor: malloc failed");
    }

    // Cleanup helper
    auto free_resources = [&]() {
        free(img_buffer);
        if (palette) free(palette);
        if (icc_profile) free(icc_profile);
    };

    // Load Pixels
    res = bmpread_load_image(in.h, &img_buffer);
    if (res != BMP_RESULT_OK) {
        std::string err = bmp_errmsg(in.h);
        if (err.empty()) err = bmplib_result_to_string(res);
        free_resources();
        Logger::log(LogLevel::Error, "Failed to load image data: " + err, processor_tag());
        throw std::runtime_error("BmpProcessor: failed to load image data");
    }

    // 2. WRITE
    ScopedBmp out(output, "wb");
    if (!out.f) {
        free_resources();
        Logger::log(LogLevel::Error, "Failed to open output BMP", processor_tag());
        throw std::runtime_error("BmpProcessor: cannot open output");
    }

    out.h = bmpwrite_new(out.f);
    if (!out.h) {
        free_resources();
        throw std::runtime_error("BmpProcessor: failed to create write handle");
    }

    // Basic Setup
    bmpwrite_set_dimensions(out.h, width, height, channels, bits);

    // 64-bit Handling
    if (is_64bit) {
        bmpwrite_set_64bit(out.h);
        Logger::log(LogLevel::Debug, "Encoding as 64-bit BMP", processor_tag());
    }

    // Metadata: Resolution
    if (preserve_metadata && (xdpi > 0 || ydpi > 0)) {
        bmpwrite_set_resolution(out.h, xdpi, ydpi);
    }

    // Metadata: ICC Profile
    if (preserve_metadata && icc_size > 0 && icc_profile) {
        bmpwrite_set_iccprofile(out.h, icc_size, icc_profile);
    }

    // Compression Logic
    if (is_indexed) {
        bmpwrite_set_palette(out.h, num_colors, palette);

        // 1-bit / 2-color optimization (Huffman 1D)
        if (num_colors <= 2) {
             // Huffman requires explicit allowance
             bmpwrite_allow_huffman(out.h);
             // Set foreground index optimization (optional but good practice)
             bmpwrite_set_huffman_img_fg_idx(out.h, 1);
             Logger::log(LogLevel::Debug, "Allowed Huffman 1D compression for 1-bit image", processor_tag());
        }

        // Auto RLE handles RLE8, RLE4 and Huffman if allowed
        bmpwrite_set_rle(out.h, BMP_RLE_AUTO);
        Logger::log(LogLevel::Debug, "Encoding as indexed with auto RLE/Huffman", processor_tag());
    } else {
        // RGB
        if (!is_64bit && channels == 3 && bits == 8) {
            // RLE24 is an option for standard 24-bit RGB (OS/2 extension)
            // We enable it to allow maximum compression.
            bmpwrite_allow_rle24(out.h);
            bmpwrite_set_rle(out.h, BMP_RLE_AUTO);
            Logger::log(LogLevel::Debug, "Allowed RLE24 compression for RGB image", processor_tag());
        } else {
            Logger::log(LogLevel::Debug, "Encoding as uncompressed RGB/RGBA", processor_tag());
        }
    }

    // Save
    if (bmpwrite_save_image(out.h, img_buffer) != BMP_RESULT_OK) {
        const std::string err = bmp_errmsg(out.h);
        free_resources();
        Logger::log(LogLevel::Error, "Bmplib write error: " + err, processor_tag());
        throw std::runtime_error("BmpProcessor: failed to write image");
    }

    free_resources();
    Logger::log(LogLevel::Info, "BMP recompression finished: " + output.string(), processor_tag());
}

std::string BmpProcessor::get_raw_checksum(const std::filesystem::path& /*file_path*/) const {
    return "";
}

bool BmpProcessor::raw_equal(const std::filesystem::path& a,
                             const std::filesystem::path& b) const {
    auto load_bytes = [](const std::filesystem::path& path, std::vector<unsigned char>& buf) -> bool {
        ScopedBmp sb(path, "rb");
        if (!sb.f) return false;
        sb.h = bmpread_new(sb.f);
        if (!sb.h) return false;

        if (bmpread_load_info(sb.h) != BMP_RESULT_OK) return false;

        const size_t sz = bmpread_buffersize(sb.h);
        if (sz == 0) return false;

        buf.resize(sz);
        unsigned char* ptr = buf.data();
        return bmpread_load_image(sb.h, &ptr) == BMP_RESULT_OK;
    };

    std::vector<unsigned char> buf_a, buf_b;
    // Note: raw_equal compares decoded pixel data. It ignores compression differences (RLE vs Raw)
    // and metadata differences, verifying only visual identity.
    if (!load_bytes(a, buf_a) || !load_bytes(b, buf_b)) {
        return false;
    }

    return buf_a == buf_b;
}

} // namespace chisel