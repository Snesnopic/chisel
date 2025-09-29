//
// Created by Giuseppe Francione on 18/09/25.
//

#include "jpeg_encoder.hpp"
#include "../utils/logger.hpp"
#include <algorithm>
#include <jpeglib.h>
#include <cstdio>
#include <stdexcept>
#include <vector>
#include <filesystem>

namespace {
    // error manager (jpeg error -> c++ exception)
    struct JpegErrorMgr {
        jpeg_error_mgr pub{};
        char msg[JMSG_LENGTH_MAX]{};
    };

    void copy_saved_markers(const j_decompress_ptr srcinfo, const j_compress_ptr dstinfo, const bool preserve_metadata) {
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

        // order by marker type
        std::ranges::sort(markers,
                          [](const auto &a, const auto &b) { return a.marker < b.marker; });

        // remove duplicates
        markers.erase(std::unique(markers.begin(), markers.end(),
                                  [](const auto &a, const auto &b) {
                                      return a.marker == b.marker && a.data == b.data;
                                  }),
                      markers.end());

        // write normalized markers
        for (const auto &m: markers) {
            jpeg_write_marker(dstinfo, m.marker, m.data.data(), m.data.size());
        }
    }

    void jpeg_error_exit_throw(const j_common_ptr cinfo) {
        auto *err = reinterpret_cast<JpegErrorMgr *>(cinfo->err);
        (*cinfo->err->format_message)(cinfo, err->msg);
        Logger::log(LogLevel::WARNING, std::string("libjpeg: ") + err->msg, "libjpeg");
        throw std::runtime_error(err->msg);
    }

    // save markers to copy from source (must be called before jpeg_read_header)
    void setup_marker_saving(const j_decompress_ptr srcinfo, const bool preserve_metadata) {
        if (preserve_metadata) {
            // save all app0...app15 markers + com to copy them later
            for (int m = 0; m < 16; ++m) {
                jpeg_save_markers(srcinfo, JPEG_APP0 + m, 0xFFFF);
            }
            jpeg_save_markers(srcinfo, JPEG_COM, 0xFFFF);
        }
    }
} // namespace

JpegEncoder::JpegEncoder(const bool preserve_metadata) {
    preserve_metadata_ = preserve_metadata;
}

bool JpegEncoder::recompress(const std::filesystem::path &input,
                             const std::filesystem::path &output) {
    Logger::log(LogLevel::INFO, "Start JPEG recompression: " + input.string(), "jpeg_encoder");

    // open input/output in c style for libjpeg apis
    FILE *infile = std::fopen(input.string().c_str(), "rb");
    if (!infile) {
        Logger::log(LogLevel::ERROR, "Cannot open JPEG input: " + input.string(), "jpeg_encoder");
        throw std::runtime_error("Cannot open JPEG input: " + input.string());
    }
    FILE *outfile = std::fopen(output.string().c_str(), "wb");
    if (!outfile) {
        Logger::log(LogLevel::ERROR, "Cannot open JPEG output: " + output.string(), "jpeg_encoder");
        std::fclose(infile);
        throw std::runtime_error("Cannot open JPEG output: " + output.string());
    }

    // compression/decompression structures
    jpeg_decompress_struct srcinfo{};
    jpeg_compress_struct dstinfo{};
    JpegErrorMgr jsrcerr{}, jdsterr{};

    // setup error manager that throws exceptions
    srcinfo.err = jpeg_std_error(&jsrcerr.pub);
    jsrcerr.pub.error_exit = jpeg_error_exit_throw;

    dstinfo.err = jpeg_std_error(&jdsterr.pub);
    jdsterr.pub.error_exit = jpeg_error_exit_throw;

    try {
        jpeg_create_decompress(&srcinfo);
        jpeg_create_compress(&dstinfo);

        // source
        jpeg_stdio_src(&srcinfo, infile);

        // decide which markers to save based on preserve_metadata_
        setup_marker_saving(&srcinfo, preserve_metadata_);

        // read header and file structure
        if (jpeg_read_header(&srcinfo, TRUE) != JPEG_HEADER_OK) {
            Logger::log(LogLevel::ERROR, "Invalid JPEG header: " + input.string(), "jpeg_encoder");
            throw std::runtime_error("Invalid JPEG header");
        }

        Logger::log(LogLevel::DEBUG,
                    std::string("JPEG ") + (srcinfo.progressive_mode ? "progressive" : "baseline"),
                    "jpeg_encoder");

        // read dct coefficients directly (not pixels) → lossless
        jvirt_barray_ptr *coef_arrays = jpeg_read_coefficients(&srcinfo);

        // copy critical parameters (dimensions, sampling, quant tables, etc.)
        jpeg_copy_critical_parameters(&srcinfo, &dstinfo);

        // if the file is progressive, enable optimized progressive rewrite
        if (srcinfo.progressive_mode) {
            jpeg_simple_progression(&dstinfo);
        }

        // huffman table optimization (lossless at bitstream level)
        dstinfo.optimize_coding = TRUE;

        // destination
        jpeg_stdio_dest(&dstinfo, outfile);

        // write coefficients as they are (no dct recoding) → lossless
        jpeg_write_coefficients(&dstinfo, coef_arrays);

        // copy saved markers (appn, com) if requested
        copy_saved_markers(&srcinfo, &dstinfo, preserve_metadata_);

        // finalize
        jpeg_finish_compress(&dstinfo);
        jpeg_finish_decompress(&srcinfo);

        // cleanup
        jpeg_destroy_compress(&dstinfo);
        jpeg_destroy_decompress(&srcinfo);

        std::fclose(infile);
        std::fclose(outfile);

        Logger::log(LogLevel::INFO, "JPEG recompression completed: " + output.string(), "jpeg_encoder");
        return true;
    } catch (...) {
        // cleanup in case of exception
        jpeg_destroy_compress(&dstinfo);
        jpeg_destroy_decompress(&srcinfo);
        std::fclose(infile);
        std::fclose(outfile);
        throw;
    }
}