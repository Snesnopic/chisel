//
// Created by Giuseppe Francione on 19/10/25.
//

#include "../../include/webp_processor.hpp"
#include "../../include/logger.hpp"
#include <webp/decode.h>
#include <webp/encode.h>
#include <webp/mux.h>
#include <webp/demux.h>
#include <stdexcept>
#include <vector>
#include <fstream>
#include <cstring>

namespace chisel {

namespace {

std::vector<uint8_t> read_file_bytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("cannot open " + path.string());
    const std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(static_cast<std::size_t>(size));
    if (size > 0 && !file.read(reinterpret_cast<char*>(data.data()), size)) {
        throw std::runtime_error("failed to read " + path.string());
    }
    return data;
}

void write_webp_data(const std::filesystem::path& output, const WebPData& data) {
    std::ofstream out(output, std::ios::binary);
    if (!out) throw std::runtime_error("cannot open output file: " + output.string());
    out.write(reinterpret_cast<const char*>(data.bytes), static_cast<long>(data.size));
}

// copies EXIF/XMP/ICCP chunks from the original bytes into the freshly-assembled mux
void copy_webp_metadata(WebPMux* mux, const std::vector<uint8_t>& original, std::string_view processor_name) {
    WebPData input_webp{ original.data(), original.size() };
    WebPMux* mux_in = WebPMuxCreate(&input_webp, 0);
    if (!mux_in) {
        Logger::log(LogLevel::Error, "WebPMuxCreate(mux_in) failed while preserving metadata", processor_name);
        return;
    }
    WebPData chunk;
    if (WebPMuxGetChunk(mux_in, "EXIF", &chunk) == WEBP_MUX_OK) WebPMuxSetChunk(mux, "EXIF", &chunk, 1);
    if (WebPMuxGetChunk(mux_in, "XMP ", &chunk) == WEBP_MUX_OK) WebPMuxSetChunk(mux, "XMP ", &chunk, 1);
    if (WebPMuxGetChunk(mux_in, "ICCP", &chunk) == WEBP_MUX_OK) WebPMuxSetChunk(mux, "ICCP", &chunk, 1);
    WebPMuxDelete(mux_in);
}

// checks whether every frame of an animated webp is lossless-encoded
bool all_frames_lossless(const WebPData& webp_data) {
    WebPDemuxer* demux = WebPDemux(&webp_data);
    if (!demux) return false;

    bool all_lossless = true;
    WebPIterator iter;
    if (WebPDemuxGetFrame(demux, 1, &iter)) {
        do {
            WebPBitstreamFeatures frame_features;
            if (WebPGetFeatures(iter.fragment.bytes, iter.fragment.size, &frame_features) != VP8_STATUS_OK ||
                frame_features.format != 2) {
                all_lossless = false;
                break;
            }
        } while (WebPDemuxNextFrame(&iter));
        WebPDemuxReleaseIterator(&iter);
    }
    WebPDemuxDelete(demux);
    return all_lossless;
}

void recompress_static(const std::vector<uint8_t>& input_data,
                       const std::filesystem::path& output,
                       const ProcessingOptions& options,
                       std::string_view processor_name) {
    int width = 0, height = 0;
    uint8_t* decoded = WebPDecodeRGBA(input_data.data(), input_data.size(), &width, &height);
    if (!decoded) throw std::runtime_error("WebpProcessor: decode failed");

    WebPConfig config;
    if (!WebPConfigInit(&config) || !WebPConfigLosslessPreset(&config, 9)) {
        WebPFree(decoded);
        throw std::runtime_error("WebpProcessor: WebPConfig init failed");
    }

    WebPPicture picture;
    if (!WebPPictureInit(&picture)) {
        WebPFree(decoded);
        throw std::runtime_error("WebpProcessor: WebPPictureInit failed");
    }
    picture.width = width;
    picture.height = height;
    picture.use_argb = 1; // lossless encoding needs argb, not chroma-subsampled yuv
    if (!WebPPictureImportRGBA(&picture, decoded, width * 4)) {
        WebPPictureFree(&picture);
        WebPFree(decoded);
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

    if (options.preserve_metadata) copy_webp_metadata(mux, input_data, processor_name);

    WebPData final_data;
    if (WebPMuxAssemble(mux, &final_data) != WEBP_MUX_OK) {
        WebPMuxDelete(mux);
        WebPMemoryWriterClear(&writer);
        throw std::runtime_error("WebpProcessor: WebPMuxAssemble failed");
    }

    try {
        write_webp_data(output, final_data);
    } catch (...) {
        WebPMuxDelete(mux);
        WebPMemoryWriterClear(&writer);
        WebPDataClear(&final_data);
        throw;
    }

    WebPMuxDelete(mux);
    WebPMemoryWriterClear(&writer);
    WebPDataClear(&final_data);
}

struct AnimDecoderGuard {
    WebPAnimDecoder* dec = nullptr;
    ~AnimDecoderGuard() { if (dec) WebPAnimDecoderDelete(dec); }
};

struct AnimEncoderGuard {
    WebPAnimEncoder* enc = nullptr;
    ~AnimEncoderGuard() { if (enc) WebPAnimEncoderDelete(enc); }
};

void recompress_animated(const std::vector<uint8_t>& input_data,
                         const std::filesystem::path& output,
                         const ProcessingOptions& options,
                         std::string_view processor_name) {
    WebPData webp_data{ input_data.data(), input_data.size() };

    // durations come from the container since WebPAnimDecoder only exposes cumulative timestamps
    std::vector<int> durations;
    {
        WebPDemuxer* demux = WebPDemux(&webp_data);
        if (!demux) throw std::runtime_error("WebpProcessor: WebPDemux failed");
        WebPIterator iter;
        if (WebPDemuxGetFrame(demux, 1, &iter)) {
            do {
                durations.push_back(iter.duration);
            } while (WebPDemuxNextFrame(&iter));
            WebPDemuxReleaseIterator(&iter);
        }
        WebPDemuxDelete(demux);
    }

    WebPAnimDecoderOptions dec_options;
    WebPAnimDecoderOptionsInit(&dec_options);
    dec_options.color_mode = MODE_RGBA;

    AnimDecoderGuard dec_guard{ WebPAnimDecoderNew(&webp_data, &dec_options) };
    if (!dec_guard.dec) throw std::runtime_error("WebpProcessor: WebPAnimDecoderNew failed");

    WebPAnimInfo anim_info;
    if (!WebPAnimDecoderGetInfo(dec_guard.dec, &anim_info)) {
        throw std::runtime_error("WebpProcessor: WebPAnimDecoderGetInfo failed");
    }

    WebPAnimEncoderOptions enc_options;
    WebPAnimEncoderOptionsInit(&enc_options);
    enc_options.anim_params.bgcolor = anim_info.bgcolor;
    enc_options.anim_params.loop_count = static_cast<int>(anim_info.loop_count);
    enc_options.minimize_size = 1; // let the encoder pick minimal per-frame regions/dispose/blend
    enc_options.allow_mixed = 0;   // keep every frame lossless

    AnimEncoderGuard enc_guard{ WebPAnimEncoderNew(static_cast<int>(anim_info.canvas_width),
                                                   static_cast<int>(anim_info.canvas_height),
                                                   &enc_options) };
    if (!enc_guard.enc) throw std::runtime_error("WebpProcessor: WebPAnimEncoderNew failed");

    WebPConfig config;
    if (!WebPConfigInit(&config) || !WebPConfigLosslessPreset(&config, 9)) {
        throw std::runtime_error("WebpProcessor: WebPConfig init failed");
    }

    int ts = 0;
    std::size_t frame_idx = 0;
    while (WebPAnimDecoderHasMoreFrames(dec_guard.dec)) {
        uint8_t* buf = nullptr;
        int decoder_timestamp = 0;
        if (!WebPAnimDecoderGetNext(dec_guard.dec, &buf, &decoder_timestamp)) {
            throw std::runtime_error("WebpProcessor: WebPAnimDecoderGetNext failed");
        }
        if (frame_idx >= durations.size()) {
            throw std::runtime_error("WebpProcessor: frame count mismatch between demuxer and anim decoder");
        }

        WebPPicture picture;
        if (!WebPPictureInit(&picture)) {
            throw std::runtime_error("WebpProcessor: WebPPictureInit failed");
        }
        picture.width = static_cast<int>(anim_info.canvas_width);
        picture.height = static_cast<int>(anim_info.canvas_height);
        picture.use_argb = 1;
        if (!WebPPictureImportRGBA(&picture, buf, static_cast<int>(anim_info.canvas_width) * 4)) {
            WebPPictureFree(&picture);
            throw std::runtime_error("WebpProcessor: WebPPictureImportRGBA failed");
        }

        const bool added = WebPAnimEncoderAdd(enc_guard.enc, &picture, ts, &config);
        WebPPictureFree(&picture);
        if (!added) {
            throw std::runtime_error(std::string("WebpProcessor: WebPAnimEncoderAdd failed: ") +
                                     WebPAnimEncoderGetError(enc_guard.enc));
        }

        ts += durations[frame_idx];
        ++frame_idx;
    }
    // final null-frame call closes out the last real frame's duration
    WebPAnimEncoderAdd(enc_guard.enc, nullptr, ts, nullptr);

    WebPData assembled{};
    if (!WebPAnimEncoderAssemble(enc_guard.enc, &assembled)) {
        throw std::runtime_error(std::string("WebpProcessor: WebPAnimEncoderAssemble failed: ") +
                                 WebPAnimEncoderGetError(enc_guard.enc));
    }

    WebPMux* mux = WebPMuxCreate(&assembled, 1);
    if (!mux) {
        WebPDataClear(&assembled);
        throw std::runtime_error("WebpProcessor: WebPMuxCreate failed");
    }

    if (options.preserve_metadata) copy_webp_metadata(mux, input_data, processor_name);

    WebPData final_data;
    if (WebPMuxAssemble(mux, &final_data) != WEBP_MUX_OK) {
        WebPMuxDelete(mux);
        WebPDataClear(&assembled);
        throw std::runtime_error("WebpProcessor: WebPMuxAssemble failed");
    }
    WebPDataClear(&assembled);

    try {
        write_webp_data(output, final_data);
    } catch (...) {
        WebPMuxDelete(mux);
        WebPDataClear(&final_data);
        throw;
    }

    WebPMuxDelete(mux);
    WebPDataClear(&final_data);
}

bool decode_webp_rgba8(const std::filesystem::path& path,
                       int& width,
                       int& height,
                       std::vector<uint8_t>& buffer) {
    std::vector<uint8_t> input_data;
    try {
        input_data = read_file_bytes(path);
    } catch (const std::exception&) {
        return false;
    }

    if (!WebPGetInfo(input_data.data(), input_data.size(), &width, &height)) {
        return false;
    }

    buffer.resize(static_cast<std::size_t>(width) * height * 4);
    const uint8_t* result = WebPDecodeRGBAInto(input_data.data(),
                                         input_data.size(),
                                         buffer.data(),
                                         buffer.size(),
                                         width * 4);

    return result != nullptr;
}

// compares two animated webps by their fully-composited, decoded frame sequence
bool animated_frames_equal(const std::vector<uint8_t>& data_a, const std::vector<uint8_t>& data_b) {
    WebPData wa{ data_a.data(), data_a.size() };
    WebPData wb{ data_b.data(), data_b.size() };

    WebPAnimDecoderOptions opts;
    WebPAnimDecoderOptionsInit(&opts);
    opts.color_mode = MODE_RGBA;

    AnimDecoderGuard da{ WebPAnimDecoderNew(&wa, &opts) };
    AnimDecoderGuard db{ WebPAnimDecoderNew(&wb, &opts) };
    if (!da.dec || !db.dec) return false;

    WebPAnimInfo ia, ib;
    if (!WebPAnimDecoderGetInfo(da.dec, &ia) || !WebPAnimDecoderGetInfo(db.dec, &ib)) return false;

    if (ia.canvas_width != ib.canvas_width || ia.canvas_height != ib.canvas_height ||
        ia.loop_count != ib.loop_count || ia.frame_count != ib.frame_count) {
        return false;
    }

    const std::size_t frame_bytes = static_cast<std::size_t>(ia.canvas_width) * ia.canvas_height * 4;

    while (WebPAnimDecoderHasMoreFrames(da.dec)) {
        if (!WebPAnimDecoderHasMoreFrames(db.dec)) return false;

        uint8_t* buf_a = nullptr;
        uint8_t* buf_b = nullptr;
        int ts_a = 0, ts_b = 0;
        if (!WebPAnimDecoderGetNext(da.dec, &buf_a, &ts_a)) return false;
        if (!WebPAnimDecoderGetNext(db.dec, &buf_b, &ts_b)) return false;

        if (ts_a != ts_b) return false;
        if (std::memcmp(buf_a, buf_b, frame_bytes) != 0) return false;
    }
    return !WebPAnimDecoderHasMoreFrames(db.dec);
}

} // namespace

void WebpProcessor::recompress(const std::filesystem::path& input,
                               const std::filesystem::path& output, const ProcessingOptions &options) {
    Logger::log(LogLevel::Debug, "Entering recompress for " + input.string(), get_name());

    std::vector<uint8_t> input_data;
    try {
        input_data = read_file_bytes(input);
    } catch (const std::exception& e) {
        Logger::log(LogLevel::Error, e.what(), get_name());
        throw std::runtime_error("WebpProcessor: cannot read input file");
    }

    WebPBitstreamFeatures features;
    if (WebPGetFeatures(input_data.data(), input_data.size(), &features) != VP8_STATUS_OK) {
        Logger::log(LogLevel::Error, "Feature detection failed for: " + input.string(), get_name());
        throw std::runtime_error("WebpProcessor: feature detection failed");
    }

    const auto skip_copy = [&](const char* reason) {
        Logger::log(LogLevel::Info, reason, get_name());
        try {
            std::filesystem::copy_file(input, output, std::filesystem::copy_options::overwrite_existing);
        } catch (const std::exception& e) {
            Logger::log(LogLevel::Error, std::string("Failed to copy webp: ") + e.what(), get_name());
            throw std::runtime_error("Failed to copy skipped WebP");
        }
    };

    try {
        if (features.has_animation) {
            WebPData webp_data{ input_data.data(), input_data.size() };
            if (!all_frames_lossless(webp_data)) {
                skip_copy("Animated webp has one or more lossy frames, skipping recompression");
            } else {
                recompress_animated(input_data, output, options, get_name());
            }
        } else if (features.format != 2) {
            skip_copy("Input is lossy webp, skipping recompression");
        } else {
            recompress_static(input_data, output, options, get_name());
        }
    } catch (const std::exception& e) {
        Logger::log(LogLevel::Error, e.what(), get_name());
        throw;
    }

    Logger::log(LogLevel::Debug, "Exiting recompress for " + output.string(), get_name());
}

bool WebpProcessor::raw_equal(const std::filesystem::path& a,
                              const std::filesystem::path& b) const {
    std::vector<uint8_t> data_a, data_b;
    try {
        data_a = read_file_bytes(a);
        data_b = read_file_bytes(b);
    } catch (const std::exception& e) {
        Logger::log(LogLevel::Warning, e.what(), get_name());
        return false;
    }

    WebPBitstreamFeatures fa, fb;
    if (WebPGetFeatures(data_a.data(), data_a.size(), &fa) != VP8_STATUS_OK ||
        WebPGetFeatures(data_b.data(), data_b.size(), &fb) != VP8_STATUS_OK) {
        return false;
    }
    if (fa.has_animation != fb.has_animation) return false;

    if (fa.has_animation) {
        return animated_frames_equal(data_a, data_b);
    }

    int wa, ha, wb, hb;
    std::vector<uint8_t> img_a, img_b;
    const bool ok_a = decode_webp_rgba8(a, wa, ha, img_a);
    const bool ok_b = decode_webp_rgba8(b, wb, hb, img_b);
    if (!ok_a || !ok_b) return false;
    if (wa != wb || ha != hb) return false;
    return img_a == img_b;
}

std::string WebpProcessor::get_raw_checksum(const std::filesystem::path&) const {
    // TODO: implement checksum of raw WebP data
    return "";
}

} // namespace chisel
