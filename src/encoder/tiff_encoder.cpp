//
// Created by Giuseppe Francione on 30/09/25.
//

#include "tiff_encoder.hpp"
#include "../utils/logger.hpp"
#include <tiffio.h>
#include <vector>

TiffEncoder::TiffEncoder(const bool preserve_metadata) {
   preserve_metadata_ = preserve_metadata;
}

// helper: copy essential + metadata tags from in to out
static void copy_tags_with_metadata(TIFF* in, TIFF* out, const bool preserve_metadata) {
    unsigned int width, height;
    unsigned short spp, bps, photometric, planar;

    TIFFGetField(in, TIFFTAG_IMAGEWIDTH, &width);
    TIFFGetField(in, TIFFTAG_IMAGELENGTH, &height);
    TIFFGetField(in, TIFFTAG_SAMPLESPERPIXEL, &spp);
    TIFFGetField(in, TIFFTAG_BITSPERSAMPLE, &bps);
    TIFFGetField(in, TIFFTAG_PHOTOMETRIC, &photometric);
    TIFFGetField(in, TIFFTAG_PLANARCONFIG, &planar);

    TIFFSetField(out, TIFFTAG_IMAGEWIDTH, width);
    TIFFSetField(out, TIFFTAG_IMAGELENGTH, height);
    TIFFSetField(out, TIFFTAG_SAMPLESPERPIXEL, spp);
    TIFFSetField(out, TIFFTAG_BITSPERSAMPLE, bps);
    TIFFSetField(out, TIFFTAG_PHOTOMETRIC, photometric);
    TIFFSetField(out, TIFFTAG_PLANARCONFIG, planar);

    // compression
    TIFFSetField(out, TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE);
    TIFFSetField(out, TIFFTAG_PREDICTOR, 2);

    // optional metadata
    if (preserve_metadata) {
        float xres, yres;
        unsigned short resunit;
        if (TIFFGetField(in, TIFFTAG_XRESOLUTION, &xres))
            TIFFSetField(out, TIFFTAG_XRESOLUTION, xres);
        if (TIFFGetField(in, TIFFTAG_YRESOLUTION, &yres))
            TIFFSetField(out, TIFFTAG_YRESOLUTION, yres);
        if (TIFFGetField(in, TIFFTAG_RESOLUTIONUNIT, &resunit))
            TIFFSetField(out, TIFFTAG_RESOLUTIONUNIT, resunit);

        // icc profile
        void const* icc_data = nullptr;
        unsigned int icc_len = 0;
        if (TIFFGetField(in, TIFFTAG_ICCPROFILE, &icc_len, &icc_data))
            TIFFSetField(out, TIFFTAG_ICCPROFILE, icc_len, icc_data);

        // exif ifd
        toff_t exif_offset;
        if (TIFFGetField(in, TIFFTAG_EXIFIFD, &exif_offset))
            TIFFSetField(out, TIFFTAG_EXIFIFD, exif_offset);

        // xmp packet
        void const* xmp_data = nullptr;
        unsigned int xmp_len = 0;
        if (TIFFGetField(in, TIFFTAG_XMLPACKET, &xmp_len, &xmp_data))
            TIFFSetField(out, TIFFTAG_XMLPACKET, xmp_len, xmp_data);
    }

    // colormap (palette images)
    unsigned short* r; unsigned short* g; unsigned short* b;
    if (TIFFGetField(in, TIFFTAG_COLORMAP, &r, &g, &b))
        TIFFSetField(out, TIFFTAG_COLORMAP, r, g, b);
}

// recompress implementation
bool TiffEncoder::recompress(const std::filesystem::path& input,
                             const std::filesystem::path& output) {
    Logger::log(LogLevel::Info, "Re-encoding " + input.string(), "TIFF");

    TIFF* in = TIFFOpen(input.string().c_str(), "r");
    if (!in) {
        Logger::log(LogLevel::Error, "Failed to open input TIFF", "TIFF");
        return false;
    }

    TIFF* out = TIFFOpen(output.string().c_str(), "w");
    if (!out) {
        Logger::log(LogLevel::Error, "Failed to open output TIFF", "TIFF");
        TIFFClose(in);
        return false;
    }

    // iterate over all directories (pages)
    do {
        TIFFCreateDirectory(out);
        copy_tags_with_metadata(in, out, preserve_metadata_);

        if (TIFFIsTiled(in)) {
            // tiled handling
            const tsize_t tile_size = TIFFTileSize(in);
            std::vector<uint8_t> buf(tile_size);

            const ttile_t tile_max = TIFFNumberOfTiles(in);
            for (ttile_t t = 0; t < tile_max; ++t) {
                const tsize_t read = TIFFReadEncodedTile(in, t, buf.data(), tile_size);
                if (read < 0) {
                    Logger::log(LogLevel::Error, "Failed to read tile", "TIFF");
                    TIFFClose(in);
                    TIFFClose(out);
                    return false;
                }
                if (TIFFWriteEncodedTile(out, t, buf.data(), read) < 0) {
                    Logger::log(LogLevel::Error, "Failed to write tile", "TIFF");
                    TIFFClose(in);
                    TIFFClose(out);
                    return false;
                }
            }
        } else {
            // strip handling
            const tsize_t strip_size = TIFFStripSize(in);
            std::vector<uint8_t> buf(strip_size);

            const tstrip_t strip_max = TIFFNumberOfStrips(in);
            for (tstrip_t s = 0; s < strip_max; ++s) {
                const tsize_t read = TIFFReadEncodedStrip(in, s, buf.data(), strip_size);
                if (read < 0) {
                    Logger::log(LogLevel::Error, "Failed to read strip", "TIFF");
                    TIFFClose(in);
                    TIFFClose(out);
                    return false;
                }
                if (TIFFWriteEncodedStrip(out, s, buf.data(), read) < 0) {
                    Logger::log(LogLevel::Error, "Failed to write strip", "TIFF");
                    TIFFClose(in);
                    TIFFClose(out);
                    return false;
                }
            }
        }

        // close current directory in output
        if (!TIFFWriteDirectory(out)) {
            Logger::log(LogLevel::Error, "Failed to write TIFF directory", "TIFF");
            TIFFClose(in);
            TIFFClose(out);
            return false;
        }

    } while (TIFFReadDirectory(in)); // move to next page

    TIFFClose(in);
    TIFFClose(out);

    Logger::log(LogLevel::Info, "Re-encoding complete", "TIFF");
    return true;
}