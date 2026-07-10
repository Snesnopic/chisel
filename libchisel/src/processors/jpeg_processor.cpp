//
// Created by Giuseppe Francione on 19/10/25.
//

#include "../../include/jpeg_processor.hpp"
#include "../../include/logger.hpp"
#include <jpeglib.h>
#include <cstdio>
#include <stdexcept>
#include <vector>
#include <algorithm>
#include <memory>
#include <csetjmp>
#include <cstring>
#include "file_utils.hpp"
namespace chisel {

namespace {

// error manager (jpeg error -> c++ exception)
struct JpegErrorMgr {
    jpeg_error_mgr pub{};
    char msg[JMSG_LENGTH_MAX]{};
    jmp_buf setjmp_buffer;
};

/**
 * @brief libjpeg error handler that jumps back on error.
 * @param cinfo Pointer to the libjpeg error context.
 */
void jpeg_error_exit_longjmp(const j_common_ptr cinfo) {
    auto *err = reinterpret_cast<JpegErrorMgr *>(cinfo->err);
    (*cinfo->err->format_message)(cinfo, err->msg);
    Logger::log(LogLevel::Warning, std::string("Libjpeg: ") + err->msg, "libjpeg");
    longjmp(err->setjmp_buffer, 1);
}

/**
 * @brief Configures libjpeg to save all metadata markers for later copying.
 * @param srcinfo The libjpeg decompression struct.
 * @param preserve_metadata If true, markers will be saved.
 */
void setup_marker_saving(const j_decompress_ptr srcinfo, const bool preserve_metadata) {
    if (preserve_metadata) {
        for (int m = 0; m < 16; ++m) {
            jpeg_save_markers(srcinfo, JPEG_APP0 + m, 0xFFFF);
        }
        jpeg_save_markers(srcinfo, JPEG_COM, 0xFFFF);
    }
}

/**
 * @brief Copies saved metadata markers from the decompressor to the compressor.
 * @param srcinfo The libjpeg decompression struct.
 * @param dstinfo The libjpeg compression struct.
 * @param preserve_metadata If true, markers will be copied.
 */
void copy_saved_markers(const j_decompress_ptr srcinfo,
                        const j_compress_ptr dstinfo,
                        const bool preserve_metadata) {
    if (!preserve_metadata) return;

    struct MarkerData {
        int marker;
        std::vector<JOCTET> data;
    };

    std::vector<MarkerData> markers;
    for (jpeg_saved_marker_ptr m = srcinfo->marker_list; m; m = m->next) {
        if ((m->marker >= JPEG_APP0 && m->marker <= JPEG_APP0 + 15) ||
            m->marker == JPEG_COM) {
            if (m->data && m->data_length > 0) {
                markers.push_back({m->marker, {m->data, m->data + m->data_length}});
            }
        }
    }

    std::ranges::stable_sort(markers,
                      [](const auto &a, const auto &b) { return a.marker < b.marker; });

    markers.erase(std::unique(markers.begin(), markers.end(),
                              [](const auto &a, const auto &b) {
                                  return a.marker == b.marker && a.data == b.data;
                              }),
                  markers.end());

    for (const auto &m: markers) {
        // jpeg_write_coefficients() already wrote its own JFIF/Adobe header; skip the source's to avoid a duplicate
        if (dstinfo->write_JFIF_header && m.marker == JPEG_APP0 &&
            m.data.size() >= 5 && memcmp(m.data.data(), "JFIF\0", 5) == 0) {
            continue;
        }
        if (dstinfo->write_Adobe_marker && m.marker == JPEG_APP0 + 14 &&
            m.data.size() >= 5 && memcmp(m.data.data(), "Adobe", 5) == 0) {
            continue;
        }
        jpeg_write_marker(dstinfo, m.marker, m.data.data(), m.data.size());
    }
}

} // namespace

void JpegProcessor::recompress(const std::filesystem::path& input,
                               const std::filesystem::path& output, const ProcessingOptions &options) {
    Logger::log(LogLevel::Debug, "Entering recompress for " + input.string(), get_name());

    unique_FILE infile(chisel::open_file(input.string().c_str(), "rb"));
    if (!infile) {
        Logger::log(LogLevel::Error, "Cannot open jpeg input: " + input.string(), get_name());
        throw std::runtime_error("Cannot open JPEG input");
    }
    unique_FILE outfile(chisel::open_file(output.string().c_str(), "wb"));
    if (!outfile) {
        // infile is closed automatically by raii
        Logger::log(LogLevel::Error, "Cannot open jpeg output: " + output.string(), get_name());
        throw std::runtime_error("Cannot open JPEG output");
    }

    jpeg_decompress_struct srcinfo{};
    jpeg_compress_struct dstinfo{};
    JpegErrorMgr jsrcerr{}, jdsterr{};

    // error handlers must be set before any possible error
    srcinfo.err = jpeg_std_error(&jsrcerr.pub);
    jsrcerr.pub.error_exit = jpeg_error_exit_longjmp;

    dstinfo.err = jpeg_std_error(&jdsterr.pub);
    jdsterr.pub.error_exit = jpeg_error_exit_longjmp;

    if (setjmp(jsrcerr.setjmp_buffer) || setjmp(jdsterr.setjmp_buffer)) {
        infile.reset();
        outfile.reset();
        Logger::log(LogLevel::Error, "Recompression failed due to libjpeg error", get_name());

        try {
            jpeg_destroy_compress(&dstinfo);
        } catch (...) {}

        try {
            jpeg_destroy_decompress(&srcinfo);
        } catch (...) {}

        throw std::runtime_error("Libjpeg error");
    }

    try {
        jpeg_create_decompress(&srcinfo);
        jpeg_create_compress(&dstinfo);

        jpeg_stdio_src(&srcinfo, infile.get());
        setup_marker_saving(&srcinfo, options.preserve_metadata);

        if (jpeg_read_header(&srcinfo, TRUE) != JPEG_HEADER_OK) {
            throw std::runtime_error("Invalid JPEG header");
        }

        Logger::log(LogLevel::Debug,
                    std::string("Jpeg ") + (srcinfo.progressive_mode ? "progressive" : "baseline"),
                    get_name());

        jvirt_barray_ptr *coef_arrays = jpeg_read_coefficients(&srcinfo);
        jpeg_copy_critical_parameters(&srcinfo, &dstinfo);

        if (srcinfo.progressive_mode) {
            jpeg_simple_progression(&dstinfo);
        }

        dstinfo.optimize_coding = TRUE;
        jpeg_stdio_dest(&dstinfo, outfile.get());
        jpeg_write_coefficients(&dstinfo, coef_arrays);

        copy_saved_markers(&srcinfo, &dstinfo, options.preserve_metadata);

        jpeg_finish_compress(&dstinfo);
        // Do NOT call jpeg_finish_decompress(&srcinfo) when using jpeg_read_coefficients
        // destroy structs on success path
        jpeg_destroy_compress(&dstinfo);
        jpeg_destroy_decompress(&srcinfo);

        infile.reset();

        // explicitly flush stdio buffer to disk before returning
        if (fflush(outfile.get()) != 0) {
            Logger::log(LogLevel::Warning, "Fflush failed for " + output.string(), get_name());
        }
        outfile.reset();

        Logger::log(LogLevel::Debug, "Exiting recompress for " + output.string(), get_name());

    } catch (const std::exception& e) {
        infile.reset();
        outfile.reset();
        Logger::log(LogLevel::Error,
                    "Recompression failed: " + std::string(e.what()),
                    get_name());

        // safely cleanup libjpeg structures
        try {
            jpeg_destroy_compress(&dstinfo);
        } catch (...) {
            Logger::log(LogLevel::Warning,
                        "Jpeg_destroy_compress threw an exception during cleanup",
                        get_name());
        }

        try {
            jpeg_destroy_decompress(&srcinfo);
        } catch (...) {
            Logger::log(LogLevel::Warning,
                        "Jpeg_destroy_decompress threw an exception during cleanup",
                        get_name());
        }
    }

    // files are closed automatically by unique_FILE destructor
}

/**
 * @brief Decodes a JPEG file into a raw pixel buffer.
 * @param path The path to the JPEG file.
 * @param width Output parameter for the image width.
 * @param height Output parameter for the image height.
 * @param channels Output parameter for the number of color channels.
 * @param buffer Output vector to store the raw pixel data.
 * @return True on successful decoding, false otherwise.
 */
static bool decode_jpeg_raw(const std::filesystem::path &path,
                            int &width,
                            int &height,
                            int &channels,
                            std::vector<unsigned char> &buffer) {
    unique_FILE infile(chisel::open_file(path.string().c_str(), "rb"));
    if (!infile) {
        return false;
    }

    jpeg_decompress_struct cinfo{};
    JpegErrorMgr jsrcerr{};

    cinfo.err = jpeg_std_error(&jsrcerr.pub);
    jsrcerr.pub.error_exit = jpeg_error_exit_longjmp;

    if (setjmp(jsrcerr.setjmp_buffer)) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }

    try {
        jpeg_create_decompress(&cinfo);
        jpeg_stdio_src(&cinfo, infile.get());

        if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
            jpeg_destroy_decompress(&cinfo);
            return false;
        }

        jpeg_start_decompress(&cinfo);

        width = static_cast<int>(cinfo.output_width);
        height = static_cast<int>(cinfo.output_height);
        channels = static_cast<int>(cinfo.output_components); // usually 3 (rgb) or 1 (grayscale)

        buffer.resize(static_cast<std::size_t>(width) * height * channels);
        unsigned char *row_ptr = buffer.data();
        const unsigned int row_stride = width * channels;

        while (cinfo.output_scanline < cinfo.output_height) {
            jpeg_read_scanlines(&cinfo, &row_ptr, 1);
            row_ptr += row_stride;
        }

        // Do not call jpeg_finish_decompress when we break early or just want to destroy
        jpeg_destroy_decompress(&cinfo);
    } catch (const std::exception &) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }

    return true;
}

bool JpegProcessor::raw_equal(const std::filesystem::path &a,
                              const std::filesystem::path &b) const {
    int wa, ha, ca;
    int wb, hb, cb;
    std::vector<unsigned char> imgA, imgB;

    const bool okA = decode_jpeg_raw(a, wa, ha, ca, imgA);
    const bool okB = decode_jpeg_raw(b, wb, hb, cb, imgB);

    if (!okA || !okB) {
        // one or both files failed to decode
        return false;
    }

    if (wa != wb || ha != hb || ca != cb) {
        // dimensions or channel count mismatch
        return false;
    }

    return imgA == imgB;
}
std::string JpegProcessor::get_raw_checksum(const std::filesystem::path&) const {
    // TODO: implement checksum of raw JPEG data
    return "";
}

} // namespace chisel