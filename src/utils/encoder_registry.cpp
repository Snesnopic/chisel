// encoder_registry.cpp
#include "encoder_registry.hpp"
#include "../encoder/ape_encoder.hpp"
#include "../encoder/flac_encoder.hpp"
#include "../encoder/gif_encoder.hpp"
#include "../encoder/png_encoder.hpp"
#include "../encoder/zopflipng_encoder.hpp"
#include "../encoder/jpeg_encoder.hpp"
#include "../encoder/jxl_encoder.hpp"
#include "../encoder/mseed_encoder.hpp"
#include "../encoder/sqlite_encoder.hpp"
#include "../encoder/tiff_encoder.hpp"
#include "../encoder/wavpack_encoder.hpp"
#include "../encoder/webp_encoder.hpp"
#include "../encoder/pdf_encoder.hpp"

EncoderRegistry build_encoder_registry(const bool preserve_metadata) {
    EncoderRegistry factories;

    factories["audio/flac"] = {
        [preserve_metadata] { return std::make_unique<FlacEncoder>(preserve_metadata); }
    };
    factories["audio/x-flac"] = factories["audio/flac"];

    factories["image/png"] = {
        [preserve_metadata] { return std::make_unique<PngEncoder>(preserve_metadata); },
        [preserve_metadata] { return std::make_unique<ZopfliPngEncoder>(preserve_metadata); }
    };

    factories["image/jpeg"] = {
        [preserve_metadata] { return std::make_unique<JpegEncoder>(preserve_metadata); }
    };
    factories["image/jpg"] = factories["image/jpeg"];

    factories["image/jxl"] = {
        [preserve_metadata] { return std::make_unique<JXLEncoder>(preserve_metadata); }
    };

    factories["application/x-sqlite3"] = {
        [preserve_metadata] { return std::make_unique<SqliteEncoder>(preserve_metadata); }
    };

    factories["image/tiff"] = {
        [preserve_metadata] { return std::make_unique<TiffEncoder>(preserve_metadata); }
    };
    factories["image/tiff-fx"] = factories["image/tiff"];

    factories["audio/x-wavpack"] = {
        [preserve_metadata] { return std::make_unique<WavpackEncoder>(preserve_metadata); }
    };
    factories["audio/x-wavpack-correction"] = factories["audio/x-wavpack"];

    factories["image/webp"] = {
        [preserve_metadata] { return std::make_unique<WebpEncoder>(preserve_metadata); }
    };
    factories["image/x-webp"] = factories["image/webp"];

    factories["application/pdf"] = {
        [preserve_metadata] { return std::make_unique<PdfEncoder>(preserve_metadata); }
    };

    factories["application/vnd.fdsn.mseed"] = {
        [preserve_metadata] { return std::make_unique<MseedEncoder>(preserve_metadata); }
    };

    factories["image/gif"] = {
        [preserve_metadata] { return std::make_unique<GifEncoder>(preserve_metadata); }
    };

    factories["audio/ape"] = {
        [preserve_metadata] { return std::make_unique<ApeEncoder>(preserve_metadata); }
    };
    factories["audio/x-ape"] = factories["audio/ape"];

    return factories;
}