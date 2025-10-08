//
// Created by Giuseppe Francione on 25/09/25.
//

#include "webp_encoder.hpp"
#include "../utils/logger.hpp"
#include <webp/decode.h>
#include <webp/encode.h>
#include <webp/mux.h>
#include <stdexcept>
#include <vector>
#include <fstream>
#include <sstream>

WebpEncoder::WebpEncoder(const bool preserve_metadata) {
    preserve_metadata_ = preserve_metadata;
}

bool WebpEncoder::recompress(const std::filesystem::path& input,
                             const std::filesystem::path& output) {
    Logger::log(LogLevel::INFO, "Starting WebP recompression: " + input.string(), "webp_encoder");

    // read input file into memory
    std::ifstream file(input, std::ios::binary | std::ios::ate);
    if (!file) {
        Logger::log(LogLevel::ERROR, "Cannot open input file: " + input.string(), "webp_encoder");
        throw std::runtime_error("cannot open input file");
    }
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> input_data(size);
    if (!file.read(reinterpret_cast<char*>(input_data.data()), size)) {
        Logger::log(LogLevel::ERROR, "Failed to read input file: " + input.string(), "webp_encoder");
        throw std::runtime_error("failed to read input file");
    }

    // inspect bitstream features to decide if recompression is meaningful
    WebPBitstreamFeatures features;
    if (WebPGetFeatures(input_data.data(), input_data.size(), &features) != VP8_STATUS_OK) {
        Logger::log(LogLevel::ERROR, "WebP feature detection failed", "webp_encoder");
        throw std::runtime_error("webp feature detection failed");
    }

    // if input is lossy, skip recompression (would only increase size)
    if (features.format != 2) { // 2 = lossless
        Logger::log(LogLevel::INFO, "Input is lossy WebP, skipping recompression", "webp_encoder");
        throw std::runtime_error("Input is lossy, can't recompress further");
    }

    // decode webp image to raw rgba
    int width = 0, height = 0;
    uint8_t* decoded = WebPDecodeRGBA(input_data.data(), input_data.size(), &width, &height);
    if (!decoded) {
        Logger::log(LogLevel::ERROR, "WebP decode failed: " + input.string(), "webp_encoder");
        throw std::runtime_error("webp decode failed");
    }

    // configure encoder for maximum lossless compression
    WebPConfig config;
    if (!WebPConfigInit(&config)) {
        WebPFree(decoded);
        Logger::log(LogLevel::ERROR, "WebPConfigInit failed", "webp_encoder");
        throw std::runtime_error("WebPConfigInit failed");
    }
    if (!WebPConfigLosslessPreset(&config, 9)) {
        WebPFree(decoded);
        Logger::log(LogLevel::ERROR, "WebPConfigLosslessPreset failed", "webp_encoder");
        throw std::runtime_error("WebPConfigLosslessPreset failed");
    }

    WebPPicture picture;
    if (!WebPPictureInit(&picture)) {
        WebPFree(decoded);
        Logger::log(LogLevel::ERROR, "WebPPictureInit failed", "webp_encoder");
        throw std::runtime_error("WebPPictureInit failed");
    }
    picture.width = width;
    picture.height = height;
    if (!WebPPictureImportRGBA(&picture, decoded, width * 4)) {
        WebPPictureFree(&picture);
        WebPFree(decoded);
        Logger::log(LogLevel::ERROR, "WebPPictureImportRGBA failed", "webp_encoder");
        throw std::runtime_error("WebPPictureImportRGBA failed");
    }
    WebPFree(decoded);

    // encode to memory
    WebPMemoryWriter writer;
    WebPMemoryWriterInit(&writer);
    picture.writer = WebPMemoryWrite;
    picture.custom_ptr = &writer;

    if (!WebPEncode(&config, &picture)) {
        WebPPictureFree(&picture);
        WebPMemoryWriterClear(&writer);
        Logger::log(LogLevel::ERROR, "WebPEncode failed", "webp_encoder");
        throw std::runtime_error("WebPEncode failed");
    }
    WebPPictureFree(&picture);

    // wrap encoded image
    WebPData output_image;
    output_image.bytes = writer.mem;
    output_image.size = writer.size;

    WebPMux* mux = WebPMuxCreate(&output_image, 1);
    if (!mux) {
        WebPMemoryWriterClear(&writer);
        Logger::log(LogLevel::ERROR, "WebPMuxCreate failed", "webp_encoder");
        throw std::runtime_error("WebPMuxCreate failed");
    }

    // if preserve_metadata is true, copy exif/xmp/icc chunks from input
    if (preserve_metadata_) {
        WebPData input_webp;
        input_webp.bytes = input_data.data();
        input_webp.size = input_data.size();
        WebPMux* mux_in = WebPMuxCreate(&input_webp, 0);
        if (mux_in) {
            WebPData chunk;
            if (WebPMuxGetChunk(mux_in, "EXIF", &chunk) == WEBP_MUX_OK) {
                WebPMuxSetChunk(mux, "EXIF", &chunk, 1);
            }
            if (WebPMuxGetChunk(mux_in, "XMP ", &chunk) == WEBP_MUX_OK) {
                WebPMuxSetChunk(mux, "XMP ", &chunk, 1);
            }
            if (WebPMuxGetChunk(mux_in, "ICCP", &chunk) == WEBP_MUX_OK) {
                WebPMuxSetChunk(mux, "ICCP", &chunk, 1);
            }
            WebPMuxDelete(mux_in);
        }
    }

    // assemble final webp
    WebPData final_data;
    if (WebPMuxAssemble(mux, &final_data) != WEBP_MUX_OK) {
        WebPMuxDelete(mux);
        WebPMemoryWriterClear(&writer);
        Logger::log(LogLevel::ERROR, "WebPMuxAssemble failed", "webp_encoder");
        throw std::runtime_error("WebPMuxAssemble failed");
    }

    // write to output file
    std::ofstream out(output, std::ios::binary);
    if (!out) {
        WebPMuxDelete(mux);
        WebPMemoryWriterClear(&writer);
        WebPDataClear(&final_data);
        Logger::log(LogLevel::ERROR, "Cannot open output file: " + output.string(), "webp_encoder");
        throw std::runtime_error("cannot open output file");
    }
    out.write(reinterpret_cast<const char*>(final_data.bytes), final_data.size);

    // cleanup
    WebPMuxDelete(mux);
    WebPMemoryWriterClear(&writer);
    WebPDataClear(&final_data);

    Logger::log(LogLevel::INFO, "WebP recompression completed: " + output.string(), "webp_encoder");
    return true;
}