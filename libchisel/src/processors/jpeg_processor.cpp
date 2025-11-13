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

namespace {

// error manager (jpeg error -> c++ exception)
struct JpegErrorMgr {
    jpeg_error_mgr pub{};
    char msg[JMSG_LENGTH_MAX]{};
};

void jpeg_error_exit_throw(const j_common_ptr cinfo) {
    auto *err = reinterpret_cast<JpegErrorMgr *>(cinfo->err);
    (*cinfo->err->format_message)(cinfo, err->msg);
    Logger::log(LogLevel::Warning, std::string("libjpeg: ") + err->msg, "libjpeg");
    throw std::runtime_error(err->msg);
}

// raii wrapper for file handles, ensures fclose is called
struct FileCloser {
    void operator()(FILE *f) const { if (f) std::fclose(f); }
};
using unique_FILE = std::unique_ptr<FILE, FileCloser>;

// save markers to copy from source (must be called before jpeg_read_header)
void setup_marker_saving(const j_decompress_ptr srcinfo, const bool preserve_metadata) {
    if (preserve_metadata) {
        for (int m = 0; m < 16; ++m) {
            jpeg_save_markers(srcinfo, JPEG_APP0 + m, 0xFFFF);
        }
        jpeg_save_markers(srcinfo, JPEG_COM, 0xFFFF);
    }
}

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

    std::ranges::sort(markers,
                      [](const auto &a, const auto &b) { return a.marker < b.marker; });

    markers.erase(std::unique(markers.begin(), markers.end(),
                              [](const auto &a, const auto &b) {
                                  return a.marker == b.marker && a.data == b.data;
                              }),
                  markers.end());

    for (const auto &m: markers) {
        jpeg_write_marker(dstinfo, m.marker, m.data.data(), m.data.size());
    }
}

} // namespace

namespace chisel {

void JpegProcessor::recompress(const std::filesystem::path& input,
                               const std::filesystem::path& output,
                               bool preserve_metadata) {
    Logger::log(LogLevel::Info, "Start JPEG recompression: " + input.string(), "jpeg_processor");

    unique_FILE infile(std::fopen(input.string().c_str(), "rb"));
    if (!infile) {
        Logger::log(LogLevel::Error, "Cannot open JPEG input: " + input.string(), "jpeg_processor");
        throw std::runtime_error("Cannot open JPEG input");
    }
    unique_FILE outfile(std::fopen(output.string().c_str(), "wb"));
    if (!outfile) {
        // infile is closed automatically by raii
        Logger::log(LogLevel::Error, "Cannot open JPEG output: " + output.string(), "jpeg_processor");
        throw std::runtime_error("Cannot open JPEG output");
    }

    jpeg_decompress_struct srcinfo{};
    jpeg_compress_struct dstinfo{};
    JpegErrorMgr jsrcerr{}, jdsterr{};

    // error handlers must be set before any possible error
    srcinfo.err = jpeg_std_error(&jsrcerr.pub);
    jsrcerr.pub.error_exit = jpeg_error_exit_throw;

    dstinfo.err = jpeg_std_error(&jdsterr.pub);
    jdsterr.pub.error_exit = jpeg_error_exit_throw;

    try {
        jpeg_create_decompress(&srcinfo);
        jpeg_create_compress(&dstinfo);

        jpeg_stdio_src(&srcinfo, infile.get());
        setup_marker_saving(&srcinfo, preserve_metadata);

        if (jpeg_read_header(&srcinfo, TRUE) != JPEG_HEADER_OK) {
            throw std::runtime_error("Invalid JPEG header");
        }

        Logger::log(LogLevel::Debug,
                    std::string("JPEG ") + (srcinfo.progressive_mode ? "progressive" : "baseline"),
                    "jpeg_processor");

        jvirt_barray_ptr *coef_arrays = jpeg_read_coefficients(&srcinfo);
        jpeg_copy_critical_parameters(&srcinfo, &dstinfo);

        if (srcinfo.progressive_mode) {
            jpeg_simple_progression(&dstinfo);
        }

        dstinfo.optimize_coding = TRUE;
        jpeg_stdio_dest(&dstinfo, outfile.get());
        jpeg_write_coefficients(&dstinfo, coef_arrays);

        copy_saved_markers(&srcinfo, &dstinfo, preserve_metadata);

        jpeg_finish_compress(&dstinfo);
        jpeg_finish_decompress(&srcinfo);

        // explicitly flush stdio buffer to disk before returning
        if (fflush(outfile.get()) != 0) {
            Logger::log(LogLevel::Warning, "fflush failed for " + output.string(), "jpeg_processor");
        }
        outfile.reset();

        // destroy structs on success path
        jpeg_destroy_compress(&dstinfo);
        jpeg_destroy_decompress(&srcinfo);

        Logger::log(LogLevel::Info, "JPEG recompression completed: " + output.string(), "jpeg_processor");

    } catch (const std::exception& e) {
        Logger::log(LogLevel::Error,
                    "JPEG recompression failed: " + std::string(e.what()),
                    "jpeg_processor");

        // safely cleanup libjpeg structures
        try {
            jpeg_destroy_compress(&dstinfo);
        } catch (...) {
            Logger::log(LogLevel::Warning,
                        "jpeg_destroy_compress threw an exception during cleanup",
                        "jpeg_processor");
        }

        try {
            jpeg_destroy_decompress(&srcinfo);
        } catch (...) {
            Logger::log(LogLevel::Warning,
                        "jpeg_destroy_decompress threw an exception during cleanup",
                        "jpeg_processor");
        }
    }

    // files are closed automatically by unique_FILE destructor
}

std::string JpegProcessor::get_raw_checksum(const std::filesystem::path&) const {
    // TODO: implement checksum of raw JPEG data
    return "";
}

} // namespace chisel