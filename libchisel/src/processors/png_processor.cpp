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
    void png_error_fn(png_structp png, const png_const_charp msg) {
        Logger::log(LogLevel::Error, std::string("libpng: ") + msg, "libpng");
        longjmp(png_jmpbuf(png), 1);
    }

    /**
     * @brief libpng warning handler.
     * @param msg The warning message from libpng.
     */
    void png_warning_fn(png_structp, const png_const_charp msg) {
        Logger::log(LogLevel::Warning, std::string("libpng: ") + msg, "libpng");
    }

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
     * @brief One decoded APNG frame (or the single image of a static PNG),
     * always as 8-bit RGBA, at its own frame-local width/height (which for
     * animation frames after the first can be a sub-region of the canvas).
     */
    struct PngFrame {
        png_uint_32 width = 0, height = 0;
        png_uint_32 x_offset = 0, y_offset = 0;
        png_uint_16 delay_num = 0, delay_den = 0;
        png_byte dispose_op = PNG_fcTL_DISPOSE_OP_NONE;
        png_byte blend_op = PNG_fcTL_BLEND_OP_SOURCE;
        bool has_fctl = false; // false only for a hidden default image
        std::vector<unsigned char> rgba;
    };

    /**
     * @brief Result of decoding a (possibly animated) PNG: canvas size, APNG
     * animation parameters if any, and every frame decoded to RGBA8.
     */
    struct PngDecoded {
        png_uint_32 canvas_width = 0, canvas_height = 0;
        bool is_animated = false;
        png_uint_32 num_frames = 0;
        png_uint_32 num_plays = 0;
        bool first_frame_hidden = false;
        std::vector<PngFrame> frames;
    };

    /**
     * @brief Reads and decodes a (possibly animated) PNG into 8-bit RGBA frames.
     * @param png The libpng read struct, positioned right after png_read_info().
     * @param info The libpng info struct.
     * @return The decoded canvas/animation metadata and per-frame pixel data.
     */
    PngDecoded read_png_frames(png_structp png, png_infop info) {
        PngDecoded result;
        int bit_depth, color_type;
        png_get_IHDR(png, info, &result.canvas_width, &result.canvas_height,
                     &bit_depth, &color_type, nullptr, nullptr, nullptr);

        if (bit_depth == 16) png_set_strip_16(png);
        if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
        if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) png_set_expand_gray_1_2_4_to_8(png);
        if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
        if (!(color_type & PNG_COLOR_MASK_ALPHA)) png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
        if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(png);

        png_read_update_info(png, info);
        // now, every row read from here on out is guaranteed to be rgba8

#ifdef PNG_APNG_SUPPORTED
        if (png_get_valid(png, info, PNG_INFO_acTL)) {
            result.is_animated = true;
            png_get_acTL(png, info, &result.num_frames, &result.num_plays);
            result.first_frame_hidden = png_get_first_frame_is_hidden(png, info) != 0;
        }
#endif

        // a hidden default image is an extra frame beyond num_frames (no fcTL of its own)
        const png_uint_32 total_images = result.is_animated
            ? (result.num_frames + (result.first_frame_hidden ? 1 : 0))
            : 1;

        result.frames.reserve(total_images);
        for (png_uint_32 i = 0; i < total_images; ++i) {
#ifdef PNG_READ_APNG_SUPPORTED
            if (result.is_animated) png_read_frame_head(png, info);
#endif
            PngFrame frame;
#ifdef PNG_APNG_SUPPORTED
            if (result.is_animated && png_get_valid(png, info, PNG_INFO_fcTL)) {
                frame.has_fctl = true;
                png_get_next_frame_fcTL(png, info, &frame.width, &frame.height,
                                        &frame.x_offset, &frame.y_offset,
                                        &frame.delay_num, &frame.delay_den,
                                        &frame.dispose_op, &frame.blend_op);
            } else
#endif
            {
                // no fcTL: hidden default image, spans the full canvas
                frame.width = result.canvas_width;
                frame.height = result.canvas_height;
            }

            const std::size_t rowbytes = static_cast<std::size_t>(frame.width) * 4;
            frame.rgba.resize(rowbytes * frame.height);
            std::vector<png_bytep> row_pointers(frame.height);
            for (png_uint_32 y = 0; y < frame.height; ++y) {
                row_pointers[y] = frame.rgba.data() + y * rowbytes;
            }

            png_read_image(png, row_pointers.data());
            result.frames.push_back(std::move(frame));
        }

        png_read_end(png, info);
        return result;
    }


    // single pass recompress with optimization
    void PngProcessor::recompress(const std::filesystem::path &input,
                                  const std::filesystem::path &output, const ProcessingOptions &options) {

        Logger::log(LogLevel::Debug, "Entering recompress for " + input.string(), get_name());

        // --- PASS 1: READ + ANALYZE ---

        unique_FILE fp_in(chisel::open_file(input.string().c_str(), "rb"));
        if (!fp_in) {
            Logger::log(LogLevel::Error, "Cannot open png input: " + input.string(), get_name());
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

        const PngDecoded decoded = read_png_frames(rd.png, rd.info);
        const png_uint_32 width = decoded.canvas_width;
        const png_uint_32 height = decoded.canvas_height;

        // plte/color-type apply to the whole file, not per-frame, so the palette must fit all frames
        bool all_gray = true;
        bool all_opaque = true;
        bool can_use_palette = true;
        std::map<uint32_t, uint8_t> color_to_index_map;
        std::vector<png_color> palette;
        std::vector<png_byte> transparency;

        for (const auto& frame : decoded.frames) {
            const unsigned char* p = frame.rgba.data();
            for (png_uint_32 y = 0; y < frame.height; ++y) {
                for (png_uint_32 x = 0; x < frame.width; ++x) {
                    unsigned char r = p[0], g = p[1], b = p[2], a = p[3];

                    if (r != g || g != b) all_gray = false;
                    if (a != 0xFF) all_opaque = false;

                    if (can_use_palette) {
                        uint32_t color = pack_rgba(r, g, b, a);
                        if (!color_to_index_map.contains(color)) {
                            if (color_to_index_map.size() >= 256) {
                                can_use_palette = false;
                            } else {
                                auto index = static_cast<uint8_t>(color_to_index_map.size());
                                color_to_index_map[color] = index;
                                palette.push_back({.red=r, .green=g, .blue=b});
                                transparency.push_back(a);
                            }
                        }
                    }
                    p += 4;
                }
            }
        }

        // --- PASS 2: WRITE ---

        const unique_FILE fp_out(chisel::open_file(output.string().c_str(), "wb"));
        if (!fp_out) {
            Logger::log(LogLevel::Error, "Cannot open png output: " + output.string(), get_name());
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

#ifdef PNG_APNG_SUPPORTED
        // animation structure is content, not metadata: always preserved regardless of preserve_metadata
        if (decoded.is_animated) {
            png_set_acTL(wr.png, wr.info, decoded.num_frames, decoded.num_plays);
            if (decoded.first_frame_hidden) {
                png_set_first_frame_is_hidden(wr.png, wr.info, 1);
            }
        }
#endif

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

            copy_metadata_if_requested(rd_meta.png, rd_meta.info, wr.png, wr.info, options.preserve_metadata);
        } // meta read structs destroyed here

        png_write_info(wr.png, wr.info);

        // output row buffer sized for the canvas, the largest any frame region can be
        const png_size_t out_channels = png_get_channels(wr.png, wr.info);
        std::vector<unsigned char> out_rowbuf(static_cast<std::size_t>(width) * out_channels * (out_bit_depth / 8));
        png_bytep out_row = out_rowbuf.data();

        for (const auto& frame : decoded.frames) {
#ifdef PNG_WRITE_APNG_SUPPORTED
            if (decoded.is_animated) {
                png_write_frame_head(wr.png, wr.info, nullptr, frame.width, frame.height,
                                     frame.x_offset, frame.y_offset,
                                     frame.delay_num, frame.delay_den,
                                     frame.dispose_op, frame.blend_op);
            }
#endif

            const unsigned char* p = frame.rgba.data();
            for (png_uint_32 y = 0; y < frame.height; ++y) {
                const unsigned char *src = p;
                unsigned char *dst = out_row;

                if (out_color_type == PNG_COLOR_TYPE_PALETTE) {
                    for (png_uint_32 x = 0; x < frame.width; ++x) {
                        uint32_t color = pack_rgba(src[0], src[1], src[2], src[3]);
                        dst[0] = color_to_index_map.at(color); // find index
                        src += 4;
                        dst += 1;
                    }
                } else if (out_color_type == PNG_COLOR_TYPE_GRAY) {
                    for (png_uint_32 x = 0; x < frame.width; ++x) {
                        dst[0] = src[0]; // r = g = b
                        src += 4;
                        dst += 1;
                    }
                } else if (out_color_type == PNG_COLOR_TYPE_GA) {
                    for (png_uint_32 x = 0; x < frame.width; ++x) {
                        dst[0] = src[0]; // r = g = b
                        dst[1] = src[3]; // alpha
                        src += 4;
                        dst += 2;
                    }
                } else if (out_color_type == PNG_COLOR_TYPE_RGB) {
                    for (png_uint_32 x = 0; x < frame.width; ++x) {
                        dst[0] = src[0]; // r
                        dst[1] = src[1]; // g
                        dst[2] = src[2]; // b
                        src += 4;
                        dst += 3;
                    }
                } else { // RGBA
                    memcpy(dst, src, static_cast<std::size_t>(frame.width) * 4);
                }

                png_write_rows(wr.png, &out_row, 1);
                p += static_cast<std::size_t>(frame.width) * 4; // advance in-memory buffer pointer
            }

#ifdef PNG_WRITE_APNG_SUPPORTED
            if (decoded.is_animated) {
                png_write_frame_tail(wr.png, wr.info);
            }
#endif
        }

        png_write_end(wr.png, wr.info);

        Logger::log(LogLevel::Debug, "Exiting recompress for " + output.string(), get_name());
    }


    std::string PngProcessor::get_raw_checksum(const std::filesystem::path &file_path) const {
        // TODO: implement checksum of raw pixel data if needed
        return "";
    }

    namespace {
        /**
         * @brief Decodes a PNG file into canvas/animation metadata and
         * per-frame RGBA8 pixel data, for use by raw_equal().
         */
        PngDecoded decode_png_file(const std::filesystem::path &file) {
            const unique_FILE fp(chisel::open_file(file.string().c_str(), "rb"));
            if (!fp) throw std::runtime_error("Cannot open PNG: " + file.string());

            PngRead rd;
            rd.png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
            if (!rd.png) throw std::runtime_error("png_create_read_struct failed");
            rd.info = png_create_info_struct(rd.png);
            if (!rd.info) throw std::runtime_error("png_create_info_struct failed");
            if (setjmp(png_jmpbuf(rd.png))) {
                throw std::runtime_error("libpng error while reading " + file.string());
            }

            png_init_io(rd.png, fp.get());
            png_read_info(rd.png, rd.info);

            return read_png_frames(rd.png, rd.info);
        }
    } // namespace

    bool PngProcessor::raw_equal(const std::filesystem::path &a,
                                 const std::filesystem::path &b) const {
        PngDecoded da, db;
        try {
            da = decode_png_file(a);
            db = decode_png_file(b);
        } catch (const std::exception& e) {
            Logger::log(LogLevel::Warning, std::string("raw_equal: failed to decode: ") + e.what(), get_name());
            return false;
        }

        if (da.canvas_width != db.canvas_width || da.canvas_height != db.canvas_height) return false;
        if (da.is_animated != db.is_animated) return false;
        if (da.is_animated && (da.num_plays != db.num_plays || da.first_frame_hidden != db.first_frame_hidden)) {
            return false;
        }
        if (da.frames.size() != db.frames.size()) return false;

        for (std::size_t i = 0; i < da.frames.size(); ++i) {
            const auto& fa = da.frames[i];
            const auto& fb = db.frames[i];
            if (fa.width != fb.width || fa.height != fb.height) return false;
            if (fa.has_fctl != fb.has_fctl) return false;
            if (fa.has_fctl && (fa.x_offset != fb.x_offset || fa.y_offset != fb.y_offset ||
                                fa.delay_num != fb.delay_num || fa.delay_den != fb.delay_den ||
                                fa.dispose_op != fb.dispose_op || fa.blend_op != fb.blend_op)) {
                return false;
            }
            if (fa.rgba != fb.rgba) return false;
        }

        return true;
    }
} // namespace chisel