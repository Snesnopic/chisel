//
// Created by Giuseppe Francione on 19/10/25.
//

#include "webp_processor.hpp"
#include <webp/decode.h>
#include <webp/encode.h>
#include <webp/mux.h>
#include <stdexcept>
#include <vector>
#include <fstream>

namespace chisel {

void WebpProcessor::recompress(const std::filesystem::path& input,
                               const std::filesystem::path& output,
                               bool preserve_metadata) {
    Logger::log(LogLevel::Info, "Starting WebP recompression: " + input.string(), "webp_processor");

    // read input file into memory
    std::ifstream file(input, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("WebpProcessor: cannot open input file");
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> input_data(size);
    if (!file.read(reinterpret_cast<char*>(input_data.data()), size)) {
        throw std::runtime_error("WebpProcessor: failed to read input file");
    }

    // inspect bitstream features
    WebPBitstreamFeatures features;
    if (WebPGetFeatures(input_data.data(), input_data.size(), &features) != VP8_STATUS_OK) {
        throw std::runtime_error("WebpProcessor: feature detection failed");
    }

    // skip lossy
    if (features.format != 2) {
        Logger::log(LogLevel::Info, "Input is lossy WebP, skipping recompression", "webp_processor");
        throw std::runtime_error("WebpProcessor: input is lossy, can't recompress further");
    }

    // decode to RGBA
    int width = 0, height = 0;
    uint8_t* decoded = WebPDecodeRGBA(input_data.data(), input_data.size(), &width, &height);
    if (!decoded) throw std::runtime_error("WebpProcessor: decode failed");

    WebPConfig config;
    if (!WebPConfigInit(&config)) { WebPFree(decoded); throw std::runtime_error("WebpProcessor: WebPConfigInit failed"); }
    if (!WebPConfigLosslessPreset(&config, 9)) { WebPFree(decoded); throw std::runtime_error("WebpProcessor: WebPConfigLosslessPreset failed"); }

    WebPPicture picture;
    if (!WebPPictureInit(&picture)) { WebPFree(decoded); throw std::runtime_error("WebpProcessor: WebPPictureInit failed"); }
    picture.width = width;
    picture.height = height;
    if (!WebPPictureImportRGBA(&picture, decoded, width * 4)) {
        WebPPictureFree(&picture); WebPFree(decoded);
        throw std::runtime_error("WebpProcessor: WebPPictureImportRGBA failed");
    }
    WebPFree(decoded);

    WebPMemoryWriter writer;
    WebPMemoryWriterInit(&writer);
    picture.writer = WebPMemoryWrite;
    picture.custom_ptr = &writer;

    if (!WebPEncode(&config, &picture)) {
        WebPPictureFree(&picture);
        WebPMemoryWriterClear(&writer);
        throw std::runtime_error("WebpProcessor: WebPEncode failed");
    }
    WebPPictureFree(&picture);

    WebPData output_image{ writer.mem, writer.size };
    WebPMux* mux = WebPMuxCreate(&output_image, 1);
    if (!mux) {
        WebPMemoryWriterClear(&writer);
        throw std::runtime_error("WebpProcessor: WebPMuxCreate failed");
    }

    if (preserve_metadata) {
        WebPData input_webp{ input_data.data(), input_data.size() };
        WebPMux* mux_in = WebPMuxCreate(&input_webp, 0);
        if (mux_in) {
            WebPData chunk;
            if (WebPMuxGetChunk(mux_in, "EXIF", &chunk) == WEBP_MUX_OK) WebPMuxSetChunk(mux, "EXIF", &chunk, 1);
            if (WebPMuxGetChunk(mux_in, "XMP ", &chunk) == WEBP_MUX_OK) WebPMuxSetChunk(mux, "XMP ", &chunk, 1);
            if (WebPMuxGetChunk(mux_in, "ICCP", &chunk) == WEBP_MUX_OK) WebPMuxSetChunk(mux, "ICCP", &chunk, 1);
            WebPMuxDelete(mux_in);
        }
    }

    WebPData final_data;
    if (WebPMuxAssemble(mux, &final_data) != WEBP_MUX_OK) {
        WebPMuxDelete(mux);
        WebPMemoryWriterClear(&writer);
        throw std::runtime_error("WebpProcessor: WebPMuxAssemble failed");
    }

    std::ofstream out(output, std::ios::binary);
    if (!out) {
        WebPMuxDelete(mux);
        WebPMemoryWriterClear(&writer);
        WebPDataClear(&final_data);
        throw std::runtime_error("WebpProcessor: cannot open output file");
    }
    out.write(reinterpret_cast<const char*>(final_data.bytes), final_data.size);

    WebPMuxDelete(mux);
    WebPMemoryWriterClear(&writer);
    WebPDataClear(&final_data);

    Logger::log(LogLevel::Info, "WebP recompression completed: " + output.string(), "webp_processor");
}

std::string WebpProcessor::get_raw_checksum(const std::filesystem::path&) const {
    // TODO: implement checksum of raw WebP data
    return "";
}

} // namespace chisel