//
// Created by Giuseppe Francione on 18/09/25.
//

#include "png_encoder.hpp"
#include "../utils/logger.hpp"
#include <png.h>
#include <zlib.h>
#include <vector>
#include <cstring> // IDE may say it's unused, but it's lying to you
#include <stdexcept>
#include <iostream>

namespace {
    void png_error_fn(png_structp, const png_const_charp msg) {
        Logger::log(LogLevel::ERROR, std::string("libpng: ") + msg, "libpng");
        throw std::runtime_error(msg);
    }

    void png_warning_fn(png_structp, const png_const_charp msg) {
        Logger::log(LogLevel::WARNING, std::string("libpng: ") + msg, "libpng");
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

    struct PngWrite {
        png_structp png = nullptr;
        png_infop info = nullptr;

        explicit PngWrite() = default;

        ~PngWrite() {
            if (png || info) png_destroy_write_struct(&png, &info);
        }
    };

    // copy selective metadata from reader to writer
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

    // set normalization transforms for reading
    // goal: always obtain 8-bit samples and rgb/a channels
    void apply_input_transforms_for_scan(png_structp png, png_infop info) {
        int bit_depth, color_type, interlace, comp, filter;
        png_uint_32 w, h;
        png_get_IHDR(png, info, &w, &h, &bit_depth, &color_type, &interlace, &comp, &filter);

        if (bit_depth == 16) png_set_strip_16(png);
        if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
        if ((color_type == PNG_COLOR_TYPE_GRAY) && bit_depth < 8) png_set_expand_gray_1_2_4_to_8(png);
        if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
        if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
            png_set_gray_to_rgb(png);
        }
        if (bit_depth < 8) png_set_packing(png);

        // add full alpha if missing (so the scan is always rgba)
        if (!(color_type & PNG_COLOR_MASK_ALPHA) && !png_get_valid(png, info, PNG_INFO_tRNS)) {
            png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
        }
    }

    // determine if a rgba row is all opaque and/or all gray
    void analyze_row_rgba(const png_bytep row, png_uint_32 width, bool &all_gray, bool &all_opaque) {
        const unsigned char *p = row;
        for (png_uint_32 x = 0; x < width; ++x) {
            unsigned char r = p[0], g = p[1], b = p[2], a = p[3];
            if (r != g || g != b) all_gray = false;
            if (a != 0xFF) all_opaque = false;
            if (!all_gray && !all_opaque) break;
            p += 4;
        }
    }
} // namespace

PngEncoder::PngEncoder(bool preserve_metadata) {
    preserve_metadata_ = preserve_metadata;
}

// two-pass streaming:
// pass 1: scan rgba8 to evaluate opaque alpha and grayscale
// pass 2: read again and write in optimized target format
bool PngEncoder::recompress(const std::filesystem::path &input,
                            const std::filesystem::path &output) {
    // pass 1: open input and scan
    {
        Logger::log(LogLevel::INFO, "Start PNG recompression: " + input.string(), "png_encoder");

        unique_FILE fp(std::fopen(input.string().c_str(), "rb"));
        if (!fp) {
            Logger::log(LogLevel::ERROR, "Cannot open PNG input: " + input.string(), "png_encoder");
            throw std::runtime_error("Cannot open PNG input (pass 1)");
        }

        PngRead rd;
        rd.png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
        if (!rd.png) {
            Logger::log(LogLevel::ERROR, "png_create_read_struct failed: " + input.string(), "png_encoder");
            throw std::runtime_error("png_create_read_struct failed (pass 1)");
        }
        png_set_error_fn(rd.png, nullptr, png_error_fn, png_warning_fn);

        rd.info = png_create_info_struct(rd.png);
        if (!rd.info) {
            Logger::log(LogLevel::ERROR, "png_create_info_struct failed: " + input.string(), "png_encoder");
            throw std::runtime_error("png_create_info_struct failed (pass 1)");
        }

        if (setjmp(png_jmpbuf(rd.png))) {
            Logger::log(LogLevel::ERROR, "libpng error: " + input.string(), "png_encoder");
            throw std::runtime_error("libpng error (pass 1)");
        }

        png_init_io(rd.png, fp.get());
        png_read_info(rd.png, rd.info);

        png_uint_32 width, height;
        int bit_depth, color_type, interlace, comp, filter;
        png_get_IHDR(rd.png, rd.info, &width, &height, &bit_depth, &color_type, &interlace, &comp, &filter);

        apply_input_transforms_for_scan(rd.png, rd.info);

        if (png_get_interlace_type(rd.png, rd.info) != PNG_INTERLACE_NONE) {
            png_set_interlace_handling(rd.png);
        }

        png_read_update_info(rd.png, rd.info);

        // now rowbytes corresponds to rgba8
        png_size_t rowbytes = png_get_rowbytes(rd.png, rd.info);
        if (rowbytes != width * 4) {
            // in theory, it should match; if interlaced, libpng handles it
        }

        std::vector<png_bytep> row_ptrs(1);
        std::vector<unsigned char> rowbuf(rowbytes);
        row_ptrs[0] = rowbuf.data();

        bool all_gray = true;
        bool all_opaque = true;

        for (png_uint_32 y = 0; y < height; ++y) {
            png_read_rows(rd.png, row_ptrs.data(), nullptr, 1);
            analyze_row_rgba(rowbuf.data(), width, all_gray, all_opaque);
            if (!all_gray && !all_opaque) {
                // no need to continue analyzing if both are already false
                // we still need to consume the image
            }
        }

        png_read_end(rd.png, rd.info);

        // save results in a temporary sidecar file? not necessary
        // store them in variables for use in pass 2: just read again
        // to pass info to pass 2, use static variables? no, we will redo the logic in step 2
        // read again from scratch and apply the target format

        // keep the decision in a local file? not necessary: we will repeat calculations in pass 2
        // to avoid double calculation, keep flags in outer scope: move pass2 right below with captured flags

        // close here and reopen for pass 2
        // execute pass 2 with calculated flags
        // to do this, use a separate block that can access all_gray/all_opaque

        // pass 2:
        {
            unique_FILE fp2(std::fopen(input.string().c_str(), "rb"));
            if (!fp2) {
                Logger::log(LogLevel::ERROR, "cannot reopen PNG input: " + input.string(), "png_encoder");
                throw std::runtime_error("cannot reopen PNG input (pass 2)");
            }

            PngRead rd2;
            rd2.png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
            if (!rd2.png) {
                Logger::log(LogLevel::ERROR, "png_create_read_struct failed: " + input.string(), "png_encoder");
                throw std::runtime_error("png_create_read_struct failed (pass 2)");
            }
            png_set_error_fn(rd2.png, nullptr, png_error_fn, png_warning_fn);

            rd2.info = png_create_info_struct(rd2.png);
            if (!rd2.info) {
                Logger::log(LogLevel::ERROR, "png_create_info_struct failed: " + input.string(), "png_encoder");
                throw std::runtime_error("png_create_info_struct failed (pass 2)");
            }
            if (setjmp(png_jmpbuf(rd2.png))) {
                Logger::log(LogLevel::ERROR, "libpng error: " + input.string(), "png_encoder");
                throw std::runtime_error("libpng error (pass 2)");
            }

            png_init_io(rd2.png, fp2.get());
            png_read_info(rd2.png, rd2.info);

            // get original dimensions
            png_uint_32 w2, h2;
            int bd2, ct2, il2, cp2, fm2;
            png_get_IHDR(rd2.png, rd2.info, &w2, &h2, &bd2, &ct2, &il2, &cp2, &fm2);

            // normalize reading based on chosen destination
            // choose output format:
            //  - if all_gray and all_opaque -> gray8
            //  - if all_gray and !all_opaque -> gray+alpha 8 (but we discovered alpha fully opaque, so !all_opaque does not occur with all_gray true)
            //  - if !all_gray and all_opaque -> rgb8
            //  - if !all_gray and !all_opaque -> rgba8
            const bool out_gray = all_gray;
            const bool out_alpha = !all_opaque;

            // apply input transforms to rgba8 for easier handling
            apply_input_transforms_for_scan(rd2.png, rd2.info);

            if (png_get_interlace_type(rd2.png, rd2.info) != PNG_INTERLACE_NONE) {
                png_set_interlace_handling(rd2.png);
            }

            png_read_update_info(rd2.png, rd2.info);
            // prepare writer
            const unique_FILE fp_out(std::fopen(output.string().c_str(), "wb"));
            if (!fp_out) {
                Logger::log(LogLevel::ERROR, "cannot open PNG output: " + output.string(), "png_encoder");
                throw std::runtime_error("cannot open PNG output");
            }

            PngWrite wr;
            wr.png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
            if (!wr.png) {
                Logger::log(LogLevel::ERROR, "png_create_write_struct failed: " + input.string(), "png_encoder");
                throw std::runtime_error("png_create_write_struct failed (writer)");
            }
            wr.info = png_create_info_struct(wr.png);
            if (!wr.info) {
                Logger::log(LogLevel::ERROR, "png_create_info_struct failed: " + input.string(), "png_encoder");
                throw std::runtime_error("png_create_info_struct failed (writer)");
            }
            if (setjmp(png_jmpbuf(wr.png))) {
                Logger::log(LogLevel::ERROR, "libpng write error: " + input.string(), "png_encoder");
                throw std::runtime_error("libpng write error");
            }

            png_init_io(wr.png, fp_out.get());

            // maximum compression
            png_set_compression_level(wr.png, 9);
            png_set_compression_mem_level(wr.png, 9);
            png_set_compression_strategy(wr.png, Z_DEFAULT_STRATEGY);
            png_set_filter(wr.png, PNG_FILTER_TYPE_BASE, PNG_ALL_FILTERS);

            int out_color_type = 0;
            if (out_gray) {
                out_color_type = out_alpha ? PNG_COLOR_TYPE_GA : PNG_COLOR_TYPE_GRAY;
            } else {
                out_color_type = out_alpha ? PNG_COLOR_TYPE_RGBA : PNG_COLOR_TYPE_RGB;
            }

            int out_bit_depth = 8;
            png_set_IHDR(wr.png, wr.info, w2, h2, out_bit_depth, out_color_type,
                         PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);

            // copy metadata if requested
            copy_metadata_if_requested(rd2.png, rd2.info, wr.png, wr.info, preserve_metadata_);

            // if not preserving metadata: nothing to copy
            // color-dependent chunks (plte/trns) are not needed because we are writing normalized rgb/gray

            png_write_info(wr.png, wr.info);

            // prepare input row buffer (always rgba8 from pipeline)
            const png_size_t in_rowbytes = png_get_rowbytes(rd2.png, rd2.info);
            std::vector<unsigned char> in_rowbuf(in_rowbytes);
            png_bytep in_row = in_rowbuf.data();

            // prepare output row buffer in target format
            const png_size_t out_channels = (out_color_type == PNG_COLOR_TYPE_GRAY)
                                          ? 1
                                          : (out_color_type == PNG_COLOR_TYPE_GA)
                                                ? 2
                                                : (out_color_type == PNG_COLOR_TYPE_RGB)
                                                      ? 3
                                                      : 4;
            std::vector<unsigned char> out_rowbuf(static_cast<size_t>(w2) * out_channels);
            png_bytep out_row = out_rowbuf.data();

            for (png_uint_32 y = 0; y < h2; ++y) {
                // read one rgba8 row
                png_read_rows(rd2.png, &in_row, nullptr, 1);

                // convert to target format without losing information
                const unsigned char *src = in_row;
                unsigned char *dst = out_row;

                if (out_color_type == PNG_COLOR_TYPE_GRAY) {
                    for (png_uint_32 x = 0; x < w2; ++x) {
                        // src = rgba
                        unsigned char r = src[0];
                        // unsigned char g = src[1];
                        // unsigned char b = src[2];
                        // since all_gray is true, r==g==b always; take r
                        dst[0] = r;
                        src += 4;
                        dst += 1;
                    }
                } else if (out_color_type == PNG_COLOR_TYPE_GA) {
                    for (png_uint_32 x = 0; x < w2; ++x) {
                        const unsigned char r = src[0];
                        // unsigned char g = src[1];
                        // unsigned char b = src[2];
                        const unsigned char a = src[3];
                        dst[0] = r; // = g = b
                        dst[1] = a;
                        src += 4;
                        dst += 2;
                    }
                } else if (out_color_type == PNG_COLOR_TYPE_RGB) {
                    for (png_uint_32 x = 0; x < w2; ++x) {
                        dst[0] = src[0];
                        dst[1] = src[1];
                        dst[2] = src[2];
                        src += 4;
                        dst += 3;
                    }
                } else {
                    // rgba
                    memcpy(dst, src, static_cast<size_t>(w2) * 4);
                }

                // write row
                png_write_rows(wr.png, &out_row, 1);
            }

            png_write_end(wr.png, wr.info);
        }
    }
    Logger::log(LogLevel::INFO, "PNG reencoding completed: " + output.string(), "png_encoder");
    return true;
}
