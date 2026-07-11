//
// Created by Giuseppe Francione on 19/10/25.
//

#include "../../include/tiff_processor.hpp"
#include "../../include/logger.hpp"
#include "../../include/file_utils.hpp"
#include <tiffio.h>
#include <vector>
#include <stdexcept>


namespace {

// helper: copy metadata tags only (compression is set separately per recompress strategy)
void copy_metadata_tags(TIFF* in, TIFF* out, const bool preserve_metadata) {
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
}

/**
 * @brief Checks whether a TIFF directory can be safely round-tripped through
 * TIFFReadRGBAImageOriented(), which always collapses everything to 8-bit RGBA.
 *
 * That's bit-exact only for already-8-bit-per-sample integer data; 16/32-bit
 * samples, floating point data, and CMYK (which undergoes an actual, lossy
 * colorimetric conversion to RGB) would otherwise be silently destroyed.
 */
bool directory_is_safe_for_rgba_conversion(TIFF* tif) {
    uint16_t bits_per_sample = 0;
    TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &bits_per_sample);
    if (bits_per_sample != 8) return false;

    uint16_t sample_format = SAMPLEFORMAT_UINT;
    TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLEFORMAT, &sample_format);
    if (sample_format == SAMPLEFORMAT_IEEEFP) return false;

    uint16_t photometric = 0;
    TIFFGetFieldDefaulted(tif, TIFFTAG_PHOTOMETRIC, &photometric);
    if (photometric == PHOTOMETRIC_SEPARATED) return false;

    return true;
}

/**
 * @brief Checks whether a directory is a low-bit-depth (1/2/4-bit) palette image.
 *
 * TIFFReadRGBAImageOriented() would decode these correctly too (colormap lookup
 * is exact), but forcing a ~8x/4x/2x larger RGBA buffer for what's fundamentally
 * a small-palette indexed image is wasteful. These are re-encoded keeping their
 * original bit depth, sample layout, and colormap -- only the compression scheme
 * changes -- which is both cheaper and losslessly exact by construction.
 */
bool directory_is_indexed_palette(TIFF* tif) {
    uint16_t bits_per_sample = 0;
    TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &bits_per_sample);
    if (bits_per_sample != 1 && bits_per_sample != 2 && bits_per_sample != 4) return false;

    uint16_t photometric = 0;
    TIFFGetFieldDefaulted(tif, TIFFTAG_PHOTOMETRIC, &photometric);
    if (photometric != PHOTOMETRIC_PALETTE) return false;

    uint16_t samples_per_pixel = 1;
    TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &samples_per_pixel);
    if (samples_per_pixel != 1) return false;

    uint16_t planar_config = PLANARCONFIG_CONTIG;
    TIFFGetFieldDefaulted(tif, TIFFTAG_PLANARCONFIG, &planar_config);
    if (planar_config != PLANARCONFIG_CONTIG) return false;

    return true;
}

/**
 * @brief Whether a directory can be pixel-compared via TIFFReadRGBAImageOriented()
 * without the comparison itself being blind to real differences -- true for both
 * the forced-RGBA recompress path and the indexed-palette recompress path, since
 * both decode exactly through this same function.
 */
bool directory_is_pixel_comparable(TIFF* tif) {
    return directory_is_safe_for_rgba_conversion(tif) || directory_is_indexed_palette(tif);
}

} // namespace

namespace chisel {

void TiffProcessor::recompress(const std::filesystem::path& input,
                               const std::filesystem::path& output, const ProcessingOptions &options) {
    Logger::log(LogLevel::Debug, "Entering recompress for " + input.string(), get_name());

    TIFF* in = TIFFOpen(input.string().c_str(), "r");
    if (!in) {
        Logger::log(LogLevel::Error, "Failed to open input tiff: " + input.string(), get_name());
        throw std::runtime_error("TiffProcessor: cannot open input");
    }

    // check every directory upfront and pick one uniform strategy for the whole file:
    // - Rgba: TIFFReadRGBAImageOriented() always collapses to 8-bit RGBA, bit-exact
    //   only for already-8-bit-per-sample integer, non-CMYK data.
    // - IndexedPalette: 1/2/4-bit palette images -- re-encoded keeping their native
    //   indexed layout and colormap (cheaper and just as exact as forcing RGBA).
    // - Verbatim: anything else (16/32-bit, floating point, CMYK, mixed pages) is
    //   copied through unchanged rather than risking silent corruption.
    enum class Strategy { Rgba, IndexedPalette, Verbatim };

    Strategy strategy = Strategy::Rgba;
    do {
        if (!directory_is_safe_for_rgba_conversion(in)) {
            strategy = Strategy::Verbatim;
            break;
        }
    } while (TIFFReadDirectory(in));

    if (strategy == Strategy::Verbatim) {
        TIFFSetDirectory(in, 0);
        bool all_indexed_palette = true;
        do {
            if (!directory_is_indexed_palette(in)) {
                all_indexed_palette = false;
                break;
            }
        } while (TIFFReadDirectory(in));
        if (all_indexed_palette) strategy = Strategy::IndexedPalette;
    }

    if (strategy == Strategy::Verbatim) {
        TIFFClose(in);
        Logger::log(LogLevel::Info,
            "Skipping " + input.filename().string() +
            ": contains samples that can't be losslessly represented as 8-bit RGBA "
            "or as a low-bit-depth palette image (16/32-bit depth, floating point, "
            "CMYK, or mixed page types)", get_name());
        std::vector<uint8_t> data;
        if (!read_file(input, data) || !write_file(output, data))
            throw std::runtime_error("TiffProcessor: failed to copy input to output");
        return;
    }

    TIFFSetDirectory(in, 0);

    TIFF* out = TIFFOpen(output.string().c_str(), "w");
    if (!out) {
        TIFFClose(in);
        Logger::log(LogLevel::Error, "Failed to open output tiff: " + output.string(), get_name());
        throw std::runtime_error("TiffProcessor: cannot open output");
    }

    do {
        uint32_t width = 0, height = 0;
        TIFFGetField(in, TIFFTAG_IMAGEWIDTH, &width);
        TIFFGetField(in, TIFFTAG_IMAGELENGTH, &height);

        if (width == 0 || height == 0) {
            Logger::log(LogLevel::Debug, "Skipping empty tiff directory", get_name());
            continue;
        }

        if (strategy == Strategy::Rgba) {
            std::vector<uint32_t> raster(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));

            // read full image into raw rgba buffer, handles decompression
            if (!TIFFReadRGBAImageOriented(in, width, height, raster.data(), ORIENTATION_TOPLEFT, 0)) {
                TIFFClose(in);
                TIFFClose(out);
                Logger::log(LogLevel::Error, "Failed to read tiff image data: " + input.string(), get_name());
                throw std::runtime_error("TiffProcessor: TIFFReadRGBAImageOriented failed");
            }

            TIFFCreateDirectory(out);
            copy_metadata_tags(in, out, options.preserve_metadata);

            // override tags for rgba output
            TIFFSetField(out, TIFFTAG_IMAGEWIDTH, width);
            TIFFSetField(out, TIFFTAG_IMAGELENGTH, height);
            TIFFSetField(out, TIFFTAG_SAMPLESPERPIXEL, 4);
            TIFFSetField(out, TIFFTAG_BITSPERSAMPLE, 8);
            TIFFSetField(out, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
            TIFFSetField(out, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
            TIFFSetField(out, TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE);
            TIFFSetField(out, TIFFTAG_PREDICTOR, 2); // horizontal differencing helps continuous-tone RGB
            TIFFSetField(out, TIFFTAG_ZIPQUALITY, 9);

            // specify alpha channel
            unsigned short extra_samples = 1;
            TIFFSetField(out, TIFFTAG_EXTRASAMPLES, 1, &extra_samples);

            for (uint32_t row = 0; row < height; ++row) {
                const tdata_t row_data = &raster[static_cast<std::size_t>(row) * width];
                if (TIFFWriteScanline(out, row_data, row) < 0) {
                    TIFFClose(in);
                    TIFFClose(out);
                    Logger::log(LogLevel::Error, "Failed to write tiff scanline for: " + output.string(), get_name());
                    throw std::runtime_error("TiffProcessor: write scanline failed");
                }
            }
        } else { // Strategy::IndexedPalette
            uint16_t bits_per_sample = 0;
            TIFFGetFieldDefaulted(in, TIFFTAG_BITSPERSAMPLE, &bits_per_sample);

            uint16_t* colormap_r = nullptr;
            uint16_t* colormap_g = nullptr;
            uint16_t* colormap_b = nullptr;
            if (!TIFFGetField(in, TIFFTAG_COLORMAP, &colormap_r, &colormap_g, &colormap_b)) {
                TIFFClose(in);
                TIFFClose(out);
                Logger::log(LogLevel::Error, "Missing colormap for palette tiff: " + input.string(), get_name());
                throw std::runtime_error("TiffProcessor: missing colormap");
            }

            TIFFCreateDirectory(out);
            copy_metadata_tags(in, out, options.preserve_metadata);

            TIFFSetField(out, TIFFTAG_IMAGEWIDTH, width);
            TIFFSetField(out, TIFFTAG_IMAGELENGTH, height);
            TIFFSetField(out, TIFFTAG_SAMPLESPERPIXEL, 1);
            TIFFSetField(out, TIFFTAG_BITSPERSAMPLE, bits_per_sample);
            TIFFSetField(out, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_PALETTE);
            TIFFSetField(out, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
            TIFFSetField(out, TIFFTAG_COLORMAP, colormap_r, colormap_g, colormap_b);
            TIFFSetField(out, TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE);
            // no predictor: horizontal differencing on arbitrary palette indices
            // (not continuous-tone samples) doesn't reflect real pixel-to-pixel
            // similarity and isn't a spec-recommended combination for this photometric
            TIFFSetField(out, TIFFTAG_ZIPQUALITY, 9);

            const tsize_t scanline_size = TIFFScanlineSize(in);
            std::vector<uint8_t> row_buf(static_cast<std::size_t>(scanline_size));
            for (uint32_t row = 0; row < height; ++row) {
                if (TIFFReadScanline(in, row_buf.data(), row) < 0) {
                    TIFFClose(in);
                    TIFFClose(out);
                    Logger::log(LogLevel::Error, "Failed to read tiff scanline: " + input.string(), get_name());
                    throw std::runtime_error("TiffProcessor: read scanline failed");
                }
                if (TIFFWriteScanline(out, row_buf.data(), row) < 0) {
                    TIFFClose(in);
                    TIFFClose(out);
                    Logger::log(LogLevel::Error, "Failed to write tiff scanline for: " + output.string(), get_name());
                    throw std::runtime_error("TiffProcessor: write scanline failed");
                }
            }
        }

        if (!TIFFWriteDirectory(out)) {
            TIFFClose(in);
            TIFFClose(out);
            Logger::log(LogLevel::Error, "Failed to write tiff directory for: " + output.string(), get_name());
            throw std::runtime_error("TiffProcessor: write directory failed");
        }

    } while (TIFFReadDirectory(in)); // handles multi-page tiffs

    TIFFClose(in);
    TIFFClose(out);

    Logger::log(LogLevel::Debug, "Exiting recompress for " + output.string(), get_name());
}

std::string TiffProcessor::get_raw_checksum(const std::filesystem::path&) const {
    // TODO: implement checksum of raw TIFF data
    return "";
}
    bool TiffProcessor::raw_equal(const std::filesystem::path& a, const std::filesystem::path& b) const {
    TIFF* in_a = TIFFOpen(a.string().c_str(), "r");
    if (!in_a) {
        Logger::log(LogLevel::Warning, "Raw_equal: Failed to open tiff: " + a.string(), get_name());
        return false;
    }

    TIFF* in_b = TIFFOpen(b.string().c_str(), "r");
    if (!in_b) {
        TIFFClose(in_a);
        Logger::log(LogLevel::Warning, "Raw_equal: Failed to open tiff: " + b.string(), get_name());
        return false;
    }

    // TIFFReadRGBAImageOriented() (used below) always collapses to 8-bit RGBA, which
    // would make this comparison blind to real differences in 16/32-bit, floating
    // point, or CMYK samples (both files would decode to the same lossy projection
    // even if their true sample data differs). Fall back to a byte compare instead.
    // Low-bit-depth palette images decode exactly through the same function (colormap
    // lookup is exact regardless of index bit width), so they're fine to pixel-compare.
    bool any_unsafe = false;
    do {
        if (!directory_is_pixel_comparable(in_a)) { any_unsafe = true; break; }
    } while (TIFFReadDirectory(in_a));
    if (!any_unsafe) {
        do {
            if (!directory_is_pixel_comparable(in_b)) { any_unsafe = true; break; }
        } while (TIFFReadDirectory(in_b));
    }
    if (any_unsafe) {
        TIFFClose(in_a);
        TIFFClose(in_b);
        std::vector<uint8_t> data_a, data_b;
        if (!read_file(a, data_a) || !read_file(b, data_b)) return false;
        return data_a == data_b;
    }
    TIFFSetDirectory(in_a, 0);
    TIFFSetDirectory(in_b, 0);

    bool same = true;
    bool more_a, more_b;

    do {
        uint32_t w_a, h_a, w_b, h_b;
        if (!TIFFGetField(in_a, TIFFTAG_IMAGEWIDTH, &w_a)) w_a = 0;
        if (!TIFFGetField(in_a, TIFFTAG_IMAGELENGTH, &h_a)) h_a = 0;
        if (!TIFFGetField(in_b, TIFFTAG_IMAGEWIDTH, &w_b)) w_b = 0;
        if (!TIFFGetField(in_b, TIFFTAG_IMAGELENGTH, &h_b)) h_b = 0;

        if (w_a != w_b || h_a != h_b) {
            Logger::log(LogLevel::Debug, "Raw_equal: dimension mismatch", get_name());
            same = false;
            break;
        }

        if (w_a == 0 || h_a == 0) { // skip empty dirs
            more_a = TIFFReadDirectory(in_a);
            more_b = TIFFReadDirectory(in_b);
            continue;
        }

        std::vector<uint32_t> raster_a(static_cast<std::size_t>(w_a) * static_cast<std::size_t>(h_a));
        std::vector<uint32_t> raster_b(static_cast<std::size_t>(w_b) * static_cast<std::size_t>(h_b));

        if (raster_a.empty()) { // both are empty, continue
             more_a = TIFFReadDirectory(in_a);
             more_b = TIFFReadDirectory(in_b);
             continue;
        }

        if (!TIFFReadRGBAImageOriented(in_a, w_a, h_a, raster_a.data(), ORIENTATION_TOPLEFT, 0)) {
            Logger::log(LogLevel::Warning, "Raw_equal: Failed to read tiff data: " + a.string(), get_name());
            same = false;
            break;
        }
        if (!TIFFReadRGBAImageOriented(in_b, w_b, h_b, raster_b.data(), ORIENTATION_TOPLEFT, 0)) {
            Logger::log(LogLevel::Warning, "Raw_equal: Failed to read tiff data: " + b.string(), get_name());
            same = false;
            break;
        }

        if (raster_a != raster_b) {
             Logger::log(LogLevel::Debug, "Raw_equal: pixel mismatch", get_name());
            same = false;
            break;
        }

        more_a = TIFFReadDirectory(in_a);
        more_b = TIFFReadDirectory(in_b);

    } while (more_a && more_b);

    if (more_a != more_b) { // one file has more pages
        Logger::log(LogLevel::Debug, "Raw_equal: page count mismatch", get_name());
        same = false;
    }

    TIFFClose(in_a);
    TIFFClose(in_b);
    return same;
}

} // namespace chisel
