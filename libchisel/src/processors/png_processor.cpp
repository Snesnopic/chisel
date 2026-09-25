//
// Created by Giuseppe Francione on 19/10/25.
//

#include "../../include/png_processor.hpp"
#include "../../include/logger.hpp"
#include "../../include/png_structure.hpp"
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
#include <array>
#include <filesystem>
#include <optional>
#include <string_view>


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
     * @brief Reads and decodes a (possibly animated) PNG into RGBA frames.
     * @param png The libpng read struct, positioned right after png_read_info().
     * @param info The libpng info struct.
     * @param rgba16 Decode to 16-bit RGBA (big-endian) instead of 8-bit RGBA, keeping 16-bit samples whole.
     * @return The decoded canvas/animation metadata and per-frame pixel data.
     */
    PngDecoded read_png_frames(png_structp png, png_infop info, const bool rgba16) {
        PngDecoded result;
        int bit_depth, color_type;
        png_get_IHDR(png, info, &result.canvas_width, &result.canvas_height,
                     &bit_depth, &color_type, nullptr, nullptr, nullptr);

        if (bit_depth == 16 && !rgba16) png_set_strip_16(png);
        if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
        if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) png_set_expand_gray_1_2_4_to_8(png);
        if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
        if (!(color_type & PNG_COLOR_MASK_ALPHA)) png_set_filler(png, rgba16 ? 0xFFFF : 0xFF, PNG_FILLER_AFTER);
        if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(png);
        if (rgba16 && bit_depth < 16) png_set_expand_16(png);
        png_set_interlace_handling(png);

        png_read_update_info(png, info);
        // now, every row read from here on out is guaranteed to be rgba8 (or rgba16)
        const std::size_t pixel_bytes = rgba16 ? 8 : 4;

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

            const std::size_t rowbytes = static_cast<std::size_t>(frame.width) * pixel_bytes;
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


    namespace png_rewrite {
    // 16-bit samples fit in 8 bits when each one repeats its high byte
    bool narrow_to_8bit(PngDecoded& decoded) {
        for (const auto& frame : decoded.frames) {
            for (std::size_t i = 0; i < frame.rgba.size(); i += 2) {
                if (frame.rgba[i] != frame.rgba[i + 1]) return false;
            }
        }
        for (auto& frame : decoded.frames) {
            std::vector<unsigned char> narrow(frame.rgba.size() / 2);
            for (std::size_t i = 0; i < narrow.size(); ++i) narrow[i] = frame.rgba[2 * i];
            frame.rgba = std::move(narrow);
        }
        return true;
    }

    void append_to_vector(png_structp png, png_bytep bytes, png_size_t size) {
        auto* out = static_cast<std::vector<unsigned char>*>(png_get_io_ptr(png));
        bool failed = false;
        try {
            out->insert(out->end(), bytes, bytes + size);
        } catch (...) {
            failed = true;
        }
        if (failed) png_error(png, "out of memory");
    }

    void flush_nothing(png_structp) {}

    // leaves the image as it is for the next stage
    void copy_unchanged(const std::filesystem::path& input, const std::filesystem::path& output,
                        const std::string_view reason, const std::string_view tag) {
        Logger::log(LogLevel::Debug, std::string(reason) + ", left as is: " + input.filename().string(), tag);
        std::error_code ec;
        std::filesystem::copy_file(input, output, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) throw std::runtime_error("Cannot copy PNG: " + ec.message());
    }
    } // namespace png_rewrite

    // single pass recompress with optimization
    void PngProcessor::recompress(const std::filesystem::path &input,
                                  const std::filesystem::path &output, const ProcessingOptions &options) {

        Logger::log(LogLevel::Debug, "Entering recompress for " + input.string(), get_name());
        using namespace png_rewrite;

        // this encoder picks its own color type, so it can't serve an image whose format is declared outside
        if (options.keep_pixel_format) {
            copy_unchanged(input, output, "Color type fixed by the container", get_name());
            return;
        }

        std::vector<unsigned char> source = chisel::read_file(input);
        const auto end = png::image_end(source);
        if (!end) throw std::runtime_error("Malformed PNG: " + input.string());
        // data after IEND isn't part of the image, but it's kept
        const std::vector<unsigned char> tail(source.begin() + static_cast<std::ptrdiff_t>(*end), source.end());
        source.resize(*end);

        const auto carried = png::carried_chunks(source, options.preserve_metadata, false);
        if (!carried) throw std::runtime_error("Malformed PNG: " + input.string());
        if (carried->blocked) {
            copy_unchanged(input, output, "Unknown chunk that can't outlive a re-encoding", get_name());
            return;
        }
        // their values can't follow a new color type or palette
        if (carried->color_bound) {
            copy_unchanged(input, output, "bKGD, sBIT, hIST or pCAL chunk", get_name());
            return;
        }

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

        const bool deep = png_get_bit_depth(rd.png, rd.info) == 16;
        PngDecoded decoded = read_png_frames(rd.png, rd.info, deep);
        if (deep && !narrow_to_8bit(decoded)) {
            copy_unchanged(input, output, "16-bit samples", get_name());
            return;
        }
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

        std::vector<unsigned char> encoded;
        PngWrite wr;
        wr.png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
        if (!wr.png) throw std::runtime_error("png_create_write_struct failed (writer)");
        wr.info = png_create_info_struct(wr.png);
        if (!wr.info) throw std::runtime_error("png_create_info_struct failed (writer)");
        if (setjmp(png_jmpbuf(wr.png))) throw std::runtime_error("libpng write error");

        png_set_write_fn(wr.png, &encoded, append_to_vector, flush_nothing);

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

        // the source's ancillary chunks go back verbatim, in their original places
        if (!png::insert_chunks(encoded, *carried)) throw std::runtime_error("Cannot insert the PNG chunks");
        encoded.insert(encoded.end(), tail.begin(), tail.end());
        if (!write_file(output, encoded)) throw std::runtime_error("Cannot write PNG output: " + output.string());

        Logger::log(LogLevel::Debug, "Exiting recompress for " + output.string(), get_name());
    }


    std::string PngProcessor::get_raw_checksum(const std::filesystem::path &file_path) const {
        // TODO: implement checksum of raw pixel data if needed
        return "";
    }

    namespace {
        /**
         * @brief Decodes a PNG file into canvas/animation metadata and
         * per-frame RGBA16 pixel data, for use by raw_equal().
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

            return read_png_frames(rd.png, rd.info, true);
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

        const auto ta = png::trailing_data(a);
        const auto tb = png::trailing_data(b);
        return ta && tb && *ta == *tb;
    }
bool PngProcessor::is_signed(const std::filesystem::path& file_path) const {
    // c2pa keeps its manifest in a caBX chunk, a digital signature goes in dSIG
    std::ifstream in(file_path, std::ios::binary);
    std::array<uint8_t, 8> head{};
    if (!in.read(reinterpret_cast<char*>(head.data()), head.size()) || std::memcmp(head.data(), "\x89PNG\r\n\x1a\n", 8) != 0) {
        return false;
    }
    while (in.read(reinterpret_cast<char*>(head.data()), head.size())) {
        const std::string_view type(reinterpret_cast<const char*>(head.data() + 4), 4);
        if (type == "caBX" || type == "dSIG") return true;
        if (type == "IEND") break;
        in.seekg(static_cast<std::streamoff>(read_be32(head.data())) + 4, std::ios::cur);
    }
    return false;
}

} // namespace chisel