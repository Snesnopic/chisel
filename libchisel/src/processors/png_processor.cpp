//
// Created by Giuseppe Francione on 19/10/25.
//

#include "../../include/png_processor.hpp"
#include "../../include/logger.hpp"
#include <png.h>
#include <zlib.h>
#include <vector>
#include <cstring> // IDE may say it's unused, but it's lying to you
#include <stdexcept>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>
#include <map>
#include "file_utils.hpp"

namespace chisel {
    /**
     * @brief libpng error handler that throws a C++ exception.
     * @param msg The error message from libpng.
     */
    void png_error_fn(png_structp, const png_const_charp msg) {
        Logger::log(LogLevel::Error, std::string("libpng: ") + msg, "libpng");
        throw std::runtime_error(msg);
    }

    /**
     * @brief libpng warning handler.
     * @param msg The warning message from libpng.
     */
    void png_warning_fn(png_structp, const png_const_charp msg) {
        Logger::log(LogLevel::Warning, std::string("libpng: ") + msg, "libpng");
    }

    /**
     * @brief RAII wrapper for FILE pointers to ensure they are closed.
     */
    struct FileCloser {
        void operator()(FILE *f) const { if (f) std::fclose(f); }
    };

    using unique_FILE = std::unique_ptr<FILE, FileCloser>;

    /**
     * @brief RAII wrapper for libpng read structs (png_structp, png_infop).
     * Ensures png_destroy_read_struct is called even if exceptions occur.
     */
    struct PngRead {
        png_structp png = nullptr;
        png_infop info = nullptr;

        explicit PngRead() = default;

        ~PngRead() {
            if (png || info) png_destroy_read_struct(&png, &info, nullptr);
        }
    };

    /**
     * @brief RAII wrapper for libpng write structs (png_structp, png_infop).
     * Ensures png_destroy_write_struct is called even if exceptions occur.
     */
    struct PngWrite {
        png_structp png = nullptr;
        png_infop info = nullptr;

        explicit PngWrite() = default;

        ~PngWrite() {
            if (png || info) png_destroy_write_struct(&png, &info);
        }
    };

    /**
     * @brief Copies ancillary chunks (metadata) from a PNG reader to a writer.
     * @param in_png The source png_structp.
     * @param in_info The source png_infop.
     * @param out_png The destination png_structp.
     * @param out_info The destination png_infop.
     * @param preserve If true, metadata is copied.
     */
    void copy_metadata_if_requested(png_structp in_png, png_infop in_info,
                                    png_structp out_png, png_infop out_info,
                                    bool preserve) {
        if (!preserve) return;
        // color profiles and gamma
        // iccp
        if (png_get_valid(in_png, in_info, PNG_INFO_iCCP)) {
            png_charp name = nullptr;
            int comp_type = 0;
            png_bytep profile = nullptr;
            png_uint_32 profile_len = 0;
            if (png_get_iCCP(in_png, in_info, &name, &comp_type, &profile, &profile_len)) {
                png_set_iCCP(out_png, out_info, name, comp_type, profile, profile_len);
            }
        }
        // srgb
        if (png_get_valid(in_png, in_info, PNG_INFO_sRGB)) {
            int intent = 0;
            if (png_get_sRGB(in_png, in_info, &intent)) {
                png_set_sRGB(out_png, out_info, intent);
            }
        }
        // gama
        if (png_get_valid(in_png, in_info, PNG_INFO_gAMA)) {
            double gamma = 0.0;
            if (png_get_gAMA(in_png, in_info, &gamma)) {
                png_set_gAMA(out_png, out_info, gamma);
            }
        }
        // chrm
        if (png_get_valid(in_png, in_info, PNG_INFO_cHRM)) {
            double wx, wy, rx, ry, gx, gy, bx, by;
            if (png_get_cHRM(in_png, in_info, &wx, &wy, &rx, &ry, &gx, &gy, &bx, &by)) {
                png_set_cHRM(out_png, out_info, wx, wy, rx, ry, gx, gy, bx, by);
            }
        }

        // sbit
        if (png_get_valid(in_png, in_info, PNG_INFO_sBIT)) {
            png_color_8p sig_bit = nullptr;
            if (png_get_sBIT(in_png, in_info, &sig_bit)) {
                png_set_sBIT(out_png, out_info, sig_bit);
            }
        }

        // phys (pixel per unit)
        if (png_get_valid(in_png, in_info, PNG_INFO_pHYs)) {
            png_uint_32 xppu = 0, yppu = 0;
            int unit = 0;
            if (png_get_pHYs(in_png, in_info, &xppu, &yppu, &unit)) {
                png_set_pHYs(out_png, out_info, xppu, yppu, unit);
            }
        }

        // splt (suggested palettes)
        int n_splt = 0;
        png_sPLT_tp splt_ptr = nullptr;
        n_splt = png_get_sPLT(in_png, in_info, &splt_ptr);
        if (n_splt > 0 && splt_ptr) {
            png_set_sPLT(out_png, out_info, splt_ptr, n_splt);
        }

        // text
        png_textp text = nullptr;
        int num_text = 0;
        png_get_text(in_png, in_info, &text, &num_text);
        if (num_text > 0 && text) {
            png_set_text(out_png, out_info, text, num_text);
        }

        // time (last timestamp)
        if (png_get_valid(in_png, in_info, PNG_INFO_tIME)) {
            png_timep mod_time = nullptr;
            if (png_get_tIME(in_png, in_info, &mod_time)) {
                png_set_tIME(out_png, out_info, mod_time);
            }
        }

        // bkgd: preserve only if compatible with output format (rgb/gray)
        if (png_get_valid(in_png, in_info, PNG_INFO_bKGD)) {
            png_color_16p bkgd = nullptr;
            if (png_get_bKGD(in_png, in_info, &bkgd)) {
                png_set_bKGD(out_png, out_info, bkgd);
            }
        }
    }

    /**
     * @brief Packs RGBA color components into a single 32-bit integer.
     * @return The packed 32-bit color value.
     */
    inline uint32_t pack_rgba(unsigned char r, unsigned char g, unsigned char b, unsigned char a) {
        return (static_cast<uint32_t>(r) << 24) |
               (static_cast<uint32_t>(g) << 16) |
               (static_cast<uint32_t>(b) << 8)  |
               (static_cast<uint32_t>(a));
    }

    /**
     * @brief Reads and decodes a PNG into a standard 8-bit RGBA buffer.
     * @param png The libpng read struct.
     * @param info The libpng info struct.
     * @param width Output parameter for the image width.
     * @param height Output parameter for the image height.
     * @return A vector containing the raw 8-bit RGBA pixel data.
     */
    std::vector<unsigned char> read_to_rgba8(png_structp png, png_infop info,
                                             png_uint_32& width, png_uint_32& height) {
        int bit_depth, color_type;
        png_get_IHDR(png, info, &width, &height, &bit_depth, &color_type, nullptr, nullptr, nullptr);

        if (bit_depth == 16) png_set_strip_16(png);
        if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
        if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) png_set_expand_gray_1_2_4_to_8(png);
        if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
        if (!(color_type & PNG_COLOR_MASK_ALPHA)) png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
        if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(png);

        png_read_update_info(png, info);
        // now, the buffer is guaranteed to be rgba8

        const size_t rowbytes = png_get_rowbytes(png, info);
        if (rowbytes != static_cast<size_t>(width) * 4) {
             throw std::runtime_error("Rowbytes mismatch, expected RGBA8");
        }

        std::vector<unsigned char> image(rowbytes * height);
        std::vector<png_bytep> row_pointers(height);
        for (png_uint_32 y = 0; y < height; ++y) {
            row_pointers[y] = image.data() + y * rowbytes;
        }

        png_read_image(png, row_pointers.data());
        png_read_end(png, info);

        return image;
    }


    // single pass recompress with optimization
    void PngProcessor::recompress(const std::filesystem::path &input,
                                  const std::filesystem::path &output,
                                  const bool preserve_metadata) {

        Logger::log(LogLevel::Info, "Start PNG recompression: " + input.string(), "png_encoder");

        // --- PASS 1: READ + ANALYZE ---

        unique_FILE fp_in(chisel::open_file(input.string().c_str(), "rb"));
        if (!fp_in) {
            Logger::log(LogLevel::Error, "Cannot open PNG input: " + input.string(), "png_encoder");
            throw std::runtime_error("Cannot open PNG input (pass 1)");
        }

        PngRead rd;
        rd.png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
        if (!rd.png) throw std::runtime_error("png_create_read_struct failed (pass 1)");
        png_set_error_fn(rd.png, nullptr, png_error_fn, png_warning_fn);

        rd.info = png_create_info_struct(rd.png);
        if (!rd.info) throw std::runtime_error("png_create_info_struct failed (pass 1)");
        if (setjmp(png_jmpbuf(rd.png))) throw std::runtime_error("libpng error (pass 1)");

        png_init_io(rd.png, fp_in.get());
        png_read_info(rd.png, rd.info);

        png_uint_32 width, height;

        std::vector<unsigned char> rgba_buffer = read_to_rgba8(rd.png, rd.info, width, height);

        // Analyze the in-memory buffer
        bool all_gray = true;
        bool all_opaque = true;
        bool can_use_palette = true;
        std::map<uint32_t, uint8_t> color_to_index_map;
        std::vector<png_color> palette;
        std::vector<png_byte> transparency;

        const unsigned char* p = rgba_buffer.data();
        for (png_uint_32 y = 0; y < height; ++y) {
            for (png_uint_32 x = 0; x < width; ++x) {
                unsigned char r = p[0], g = p[1], b = p[2], a = p[3];

                if (r != g || g != b) all_gray = false;
                if (a != 0xFF) all_opaque = false;

                if (can_use_palette) {
                    uint32_t color = pack_rgba(r, g, b, a);
                    if (color_to_index_map.find(color) == color_to_index_map.end()) {
                        if (color_to_index_map.size() >= 256) {
                            can_use_palette = false;
                        } else {
                            uint8_t index = static_cast<uint8_t>(color_to_index_map.size());
                            color_to_index_map[color] = index;
                            palette.push_back({r, g, b});
                            transparency.push_back(a);
                        }
                    }
                }
                p += 4;
            }
        }

        // --- PASS 2: WRITE ---

        const unique_FILE fp_out(chisel::open_file(output.string().c_str(), "wb"));
        if (!fp_out) {
            Logger::log(LogLevel::Error, "Cannot open PNG output: " + output.string(), "png_encoder");
            throw std::runtime_error("Cannot open PNG output");
        }

        PngWrite wr;
        wr.png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
        if (!wr.png) throw std::runtime_error("png_create_write_struct failed (writer)");
        wr.info = png_create_info_struct(wr.png);
        if (!wr.info) throw std::runtime_error("png_create_info_struct failed (writer)");
        if (setjmp(png_jmpbuf(wr.png))) throw std::runtime_error("libpng write error");

        png_init_io(wr.png, fp_out.get());

        // set max compression
        png_set_compression_level(wr.png, 9);
        png_set_compression_mem_level(wr.png, 9);
        png_set_compression_strategy(wr.png, Z_DEFAULT_STRATEGY);
        png_set_filter(wr.png, PNG_FILTER_TYPE_BASE, PNG_ALL_FILTERS);

        // determine optimal output format
        int out_color_type = 0;
        int out_bit_depth = 8;

        if (can_use_palette) {
            out_color_type = PNG_COLOR_TYPE_PALETTE;
            out_bit_depth = 8;
        } else if (all_gray && all_opaque) {
            out_color_type = PNG_COLOR_TYPE_GRAY;
        } else if (all_gray) {
            out_color_type = PNG_COLOR_TYPE_GA;
        } else if (all_opaque) {
            out_color_type = PNG_COLOR_TYPE_RGB;
        } else {
            out_color_type = PNG_COLOR_TYPE_RGBA;
        }

        png_set_IHDR(wr.png, wr.info, width, height, out_bit_depth, out_color_type,
                     PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);

        // write palette if we chose that format
        if (out_color_type == PNG_COLOR_TYPE_PALETTE) {
            png_set_PLTE(wr.png, wr.info, palette.data(), static_cast<int>(palette.size()));
            // only write tRNS if there is actual transparency
            if (!all_opaque) {
                png_set_tRNS(wr.png, wr.info, transparency.data(), static_cast<int>(transparency.size()), nullptr);
            }
        }

        // copy metadata (must be done *before* png_write_info)
        // re-open read struct to get metadata
        {
            unique_FILE fp_in_meta(chisel::open_file(input.string().c_str(), "rb"));
            PngRead rd_meta;
            rd_meta.png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
            if (!rd_meta.png) throw std::runtime_error("png_create_read_struct failed (meta)");
            rd_meta.info = png_create_info_struct(rd_meta.png);
            if (!rd_meta.info) throw std::runtime_error("png_create_info_struct failed (meta)");
            if (setjmp(png_jmpbuf(rd_meta.png))) throw std::runtime_error("libpng error (meta)");
            png_init_io(rd_meta.png, fp_in_meta.get());
            png_read_info(rd_meta.png, rd_meta.info);

            copy_metadata_if_requested(rd_meta.png, rd_meta.info, wr.png, wr.info, preserve_metadata);
        } // meta read structs destroyed here

        png_write_info(wr.png, wr.info);

        // prepare output row buffer
        const png_size_t out_channels = png_get_channels(wr.png, wr.info);
        std::vector<unsigned char> out_rowbuf(static_cast<size_t>(width) * out_channels * (out_bit_depth / 8));
        png_bytep out_row = out_rowbuf.data();

        // re-point to the start of the in-memory buffer
        p = rgba_buffer.data();

        for (png_uint_32 y = 0; y < height; ++y) {
            const unsigned char *src = p;
            unsigned char *dst = out_row;

            if (out_color_type == PNG_COLOR_TYPE_PALETTE) {
                for (png_uint_32 x = 0; x < width; ++x) {
                    uint32_t color = pack_rgba(src[0], src[1], src[2], src[3]);
                    dst[0] = color_to_index_map.at(color); // find index
                    src += 4;
                    dst += 1;
                }
            } else if (out_color_type == PNG_COLOR_TYPE_GRAY) {
                for (png_uint_32 x = 0; x < width; ++x) {
                    dst[0] = src[0]; // r = g = b
                    src += 4;
                    dst += 1;
                }
            } else if (out_color_type == PNG_COLOR_TYPE_GA) {
                for (png_uint_32 x = 0; x < width; ++x) {
                    dst[0] = src[0]; // r = g = b
                    dst[1] = src[3]; // alpha
                    src += 4;
                    dst += 2;
                }
            } else if (out_color_type == PNG_COLOR_TYPE_RGB) {
                for (png_uint_32 x = 0; x < width; ++x) {
                    dst[0] = src[0]; // r
                    dst[1] = src[1]; // g
                    dst[2] = src[2]; // b
                    src += 4;
                    dst += 3;
                }
            } else { // RGBA
                memcpy(dst, src, static_cast<size_t>(width) * 4);
            }

            png_write_rows(wr.png, &out_row, 1);
            p += static_cast<size_t>(width) * 4; // advance in-memory buffer pointer
        }

        png_write_end(wr.png, wr.info);

        Logger::log(LogLevel::Info, "PNG reencoding completed: " + output.string(), "png_encoder");
    }


    std::string PngProcessor::get_raw_checksum(const std::filesystem::path &file_path) const {
        // TODO: implement checksum of raw pixel data if needed
        return "";
    }

    /**
     * @brief Decodes a PNG file into a standard 8-bit RGBA buffer.
     * This is a standalone utility function used for checksum verification.
     * @param file The path to the PNG file.
     * @param width Output parameter for the image width.
     * @param height Output parameter for the image height.
     * @return A vector containing the raw 8-bit RGBA pixel data.
     */
    std::vector<unsigned char> decode_png_rgba8(const std::filesystem::path &file,
                                                png_uint_32 &width,
                                                png_uint_32 &height) {
        FILE *fp = chisel::open_file(file.string().c_str(), "rb");
        if (!fp) throw std::runtime_error("Cannot open PNG: " + file.string());

        png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
        if (!png) {
            fclose(fp);
            throw std::runtime_error("png_create_read_struct failed");
        }

        png_infop info = png_create_info_struct(png);
        if (!info) {
            png_destroy_read_struct(&png, nullptr, nullptr);
            fclose(fp);
            throw std::runtime_error("png_create_info_struct failed");
        }

        if (setjmp(png_jmpbuf(png))) {
            png_destroy_read_struct(&png, &info, nullptr);
            fclose(fp);
            throw std::runtime_error("libpng error while reading " + file.string());
        }

        png_init_io(png, fp);
        png_read_info(png, info);

        int bit_depth, color_type;
        png_get_IHDR(png, info, &width, &height, &bit_depth, &color_type, nullptr, nullptr, nullptr);

        if (bit_depth == 16) png_set_strip_16(png);
        if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
        if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) png_set_expand_gray_1_2_4_to_8(png);
        if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
        if (!(color_type & PNG_COLOR_MASK_ALPHA)) png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
        if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(png);

        png_read_update_info(png, info);

        const size_t rowbytes = png_get_rowbytes(png, info);
        std::vector<unsigned char> image(rowbytes * height);
        std::vector<png_bytep> row_pointers(height);
        for (png_uint_32 y = 0; y < height; ++y) {
            row_pointers[y] = image.data() + y * rowbytes;
        }

        png_read_image(png, row_pointers.data());
        png_read_end(png, info);

        png_destroy_read_struct(&png, &info, nullptr);
        fclose(fp);

        return image;
    }

    bool PngProcessor::raw_equal(const std::filesystem::path &a,
                                 const std::filesystem::path &b) const {
        png_uint_32 wa, ha, wb, hb;
        const auto imgA = decode_png_rgba8(a, wa, ha);
        const auto imgB = decode_png_rgba8(b, wb, hb);

        if (wa != wb || ha != hb) return false;
        return imgA == imgB;
    }
} // namespace chisel