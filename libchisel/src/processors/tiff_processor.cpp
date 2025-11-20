//
// Created by Giuseppe Francione on 19/10/25.
//

#include "../../include/tiff_processor.hpp"
#include "../../include/logger.hpp"
#include <tiffio.h>
#include <vector>
#include <stdexcept>

namespace {

// helper: copy metadata and set compression tags
void copy_tags_with_metadata(TIFF* in, TIFF* out, const bool preserve_metadata) {
    // set max compression
    TIFFSetField(out, TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE);
    TIFFSetField(out, TIFFTAG_PREDICTOR, 2);
    TIFFSetField(out, TIFFTAG_ZIPQUALITY, 9); // max zlib level

    if (preserve_metadata) {
        float xres, yres;
        unsigned short resunit;
        if (TIFFGetField(in, TIFFTAG_XRESOLUTION, &xres))
            TIFFSetField(out, TIFFTAG_XRESOLUTION, xres);
        if (TIFFGetField(in, TIFFTAG_YRESOLUTION, &yres))
            TIFFSetField(out, TIFFTAG_YRESOLUTION, yres);
        if (TIFFGetField(in, TIFFTAG_RESOLUTIONUNIT, &resunit))
            TIFFSetField(out, TIFFTAG_RESOLUTIONUNIT, resunit);

        void const* icc_data = nullptr;
        unsigned int icc_len = 0;
        if (TIFFGetField(in, TIFFTAG_ICCPROFILE, &icc_len, &icc_data))
            TIFFSetField(out, TIFFTAG_ICCPROFILE, icc_len, icc_data);

        toff_t exif_offset;
        if (TIFFGetField(in, TIFFTAG_EXIFIFD, &exif_offset))
            TIFFSetField(out, TIFFTAG_EXIFIFD, exif_offset);

        void const* xmp_data = nullptr;
        unsigned int xmp_len = 0;
        if (TIFFGetField(in, TIFFTAG_XMLPACKET, &xmp_len, &xmp_data))
            TIFFSetField(out, TIFFTAG_XMLPACKET, xmp_len, xmp_data);
    }
    // note: color map is intentionally skipped as we convert to rgba
}

} // namespace

namespace chisel {

void TiffProcessor::recompress(const std::filesystem::path& input,
                               const std::filesystem::path& output,
                               bool preserve_metadata) {
    Logger::log(LogLevel::Info, "Re-encoding " + input.string(), "tiff_processor");

    TIFF* in = TIFFOpen(input.string().c_str(), "r");
    if (!in) {
        Logger::log(LogLevel::Error, "Failed to open input TIFF: " + input.string(), "tiff_processor");
        throw std::runtime_error("TiffProcessor: cannot open input");
    }

    TIFF* out = TIFFOpen(output.string().c_str(), "w");
    if (!out) {
        TIFFClose(in);
        Logger::log(LogLevel::Error, "Failed to open output TIFF: " + output.string(), "tiff_processor");
        throw std::runtime_error("TiffProcessor: cannot open output");
    }

    do {
        uint32_t width, height;
        TIFFGetField(in, TIFFTAG_IMAGEWIDTH, &width);
        TIFFGetField(in, TIFFTAG_IMAGELENGTH, &height);

        std::vector<uint32_t> raster(static_cast<size_t>(width) * static_cast<size_t>(height));
        if (raster.empty()) {
            Logger::log(LogLevel::Debug, "Skipping empty TIFF directory", "tiff_processor");
            continue;
        }

        // read full image into raw rgba buffer, handles decompression
        if (!TIFFReadRGBAImageOriented(in, width, height, raster.data(), ORIENTATION_TOPLEFT, 0)) {
            TIFFClose(in);
            TIFFClose(out);
            Logger::log(LogLevel::Error, "Failed to read TIFF image data: " + input.string(), "tiff_processor");
            throw std::runtime_error("TiffProcessor: TIFFReadRGBAImageOriented failed");
        }

        TIFFCreateDirectory(out);
        copy_tags_with_metadata(in, out, preserve_metadata);

        // override tags for rgba output
        TIFFSetField(out, TIFFTAG_IMAGEWIDTH, width);
        TIFFSetField(out, TIFFTAG_IMAGELENGTH, height);
        TIFFSetField(out, TIFFTAG_SAMPLESPERPIXEL, 4);
        TIFFSetField(out, TIFFTAG_BITSPERSAMPLE, 8);
        TIFFSetField(out, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
        TIFFSetField(out, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);

        // specify alpha channel
        unsigned short extra_samples = 1;
        TIFFSetField(out, TIFFTAG_EXTRASAMPLES, 1, &extra_samples);

        for (uint32_t row = 0; row < height; ++row) {
            tdata_t row_data = &raster[static_cast<size_t>(row) * width];
            if (TIFFWriteScanline(out, row_data, row) < 0) {
                TIFFClose(in);
                TIFFClose(out);
                Logger::log(LogLevel::Error, "Failed to write TIFF scanline for: " + output.string(), "tiff_processor");
                throw std::runtime_error("TiffProcessor: write scanline failed");
            }
        }

        if (!TIFFWriteDirectory(out)) {
            TIFFClose(in);
            TIFFClose(out);
            Logger::log(LogLevel::Error, "Failed to write TIFF directory for: " + output.string(), "tiff_processor");
            throw std::runtime_error("TiffProcessor: write directory failed");
        }

    } while (TIFFReadDirectory(in)); // handles multi-page tiffs

    TIFFClose(in);
    TIFFClose(out);

    Logger::log(LogLevel::Info, "Re-encoding complete: " + output.string(), "tiff_processor");
}

std::string TiffProcessor::get_raw_checksum(const std::filesystem::path&) const {
    // TODO: implement checksum of raw TIFF data
    return "";
}
    bool TiffProcessor::raw_equal(const std::filesystem::path &a, const std::filesystem::path &b) const {
    TIFF* in_a = TIFFOpen(a.string().c_str(), "r");
    if (!in_a) {
        Logger::log(LogLevel::Warning, "raw_equal: Failed to open TIFF: " + a.string(), "tiff_processor");
        return false;
    }

    TIFF* in_b = TIFFOpen(b.string().c_str(), "r");
    if (!in_b) {
        TIFFClose(in_a);
        Logger::log(LogLevel::Warning, "raw_equal: Failed to open TIFF: " + b.string(), "tiff_processor");
        return false;
    }

    bool same = true;
    bool more_a, more_b;

    do {
        uint32_t w_a, h_a, w_b, h_b;
        if (!TIFFGetField(in_a, TIFFTAG_IMAGEWIDTH, &w_a)) w_a = 0;
        if (!TIFFGetField(in_a, TIFFTAG_IMAGELENGTH, &h_a)) h_a = 0;
        if (!TIFFGetField(in_b, TIFFTAG_IMAGEWIDTH, &w_b)) w_b = 0;
        if (!TIFFGetField(in_b, TIFFTAG_IMAGELENGTH, &h_b)) h_b = 0;

        if (w_a != w_b || h_a != h_b) {
            Logger::log(LogLevel::Debug, "raw_equal: dimension mismatch", "tiff_processor");
            same = false;
            break;
        }

        if (w_a == 0 || h_a == 0) { // skip empty dirs
            more_a = TIFFReadDirectory(in_a);
            more_b = TIFFReadDirectory(in_b);
            continue;
        }

        std::vector<uint32_t> raster_a(static_cast<size_t>(w_a) * static_cast<size_t>(h_a));
        std::vector<uint32_t> raster_b(static_cast<size_t>(w_b) * static_cast<size_t>(h_b));

        if (raster_a.empty()) { // both are empty, continue
             more_a = TIFFReadDirectory(in_a);
             more_b = TIFFReadDirectory(in_b);
             continue;
        }

        if (!TIFFReadRGBAImageOriented(in_a, w_a, h_a, raster_a.data(), ORIENTATION_TOPLEFT, 0)) {
            Logger::log(LogLevel::Warning, "raw_equal: Failed to read TIFF data: " + a.string(), "tiff_processor");
            same = false;
            break;
        }
        if (!TIFFReadRGBAImageOriented(in_b, w_b, h_b, raster_b.data(), ORIENTATION_TOPLEFT, 0)) {
            Logger::log(LogLevel::Warning, "raw_equal: Failed to read TIFF data: " + b.string(), "tiff_processor");
            same = false;
            break;
        }

        if (raster_a != raster_b) {
             Logger::log(LogLevel::Debug, "raw_equal: pixel mismatch", "tiff_processor");
            same = false;
            break;
        }

        more_a = TIFFReadDirectory(in_a);
        more_b = TIFFReadDirectory(in_b);

    } while (more_a && more_b);

    if (more_a != more_b) { // one file has more pages
        Logger::log(LogLevel::Debug, "raw_equal: page count mismatch", "tiff_processor");
        same = false;
    }

    TIFFClose(in_a);
    TIFFClose(in_b);
    return same;
}

} // namespace chisel
