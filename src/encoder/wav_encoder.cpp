//
// Created by Giuseppe Francione on 25/09/25.
//

#include "wav_encoder.hpp"
#include "../utils/logger.hpp"
#include <wavpack.h>
#include <stdexcept>
#include <vector>
#include <cstdio>

WavEncoder::WavEncoder(const bool preserve_metadata) {
    preserve_metadata_ = preserve_metadata;
}

// recompress a wav or wavpack file into wavpack lossless with maximum compression
bool WavEncoder::recompress(const std::filesystem::path& input,
                            const std::filesystem::path& output) {
    Logger::log(LogLevel::INFO, "Starting WavPack recompression: " + input.string(), "wavpack_encoder");

    // open input file
    FILE* in = std::fopen(input.string().c_str(), "rb");
    if (!in) {
        Logger::log(LogLevel::ERROR, "Cannot open input file: " + input.string(), "wavpack_encoder");
        throw std::runtime_error("Cannot open input file");
    }

    char error[80]{};
    // open input context (wav or wavpack)
    WavpackContext* ctx_in = WavpackOpenFileInput(input.string().c_str(), error, OPEN_TAGS, 0);
    if (!ctx_in) {
        std::fclose(in);
        Logger::log(LogLevel::ERROR, std::string("WavPack open failed: ") + error, "wavpack_encoder");
        throw std::runtime_error("WavPack open failed");
    }

    // prepare output
    FILE* out = std::fopen(output.string().c_str(), "wb");
    if (!out) {
        WavpackCloseFile(ctx_in);
        std::fclose(in);
        Logger::log(LogLevel::ERROR, "Cannot open output file: " + output.string(), "wavpack_encoder");
        throw std::runtime_error("Cannot open output file");
    }

    // configure encoder
    WavpackConfig config{};
    config.bytes_per_sample = WavpackGetBytesPerSample(ctx_in);
    config.bits_per_sample  = WavpackGetBitsPerSample(ctx_in);
    config.num_channels     = WavpackGetNumChannels(ctx_in);
    config.sample_rate      = WavpackGetSampleRate(ctx_in);

    // enforce maximum compression, lossless
    config.flags |= CONFIG_HIGH_FLAG;
    config.flags |= CONFIG_VERY_HIGH_FLAG;

    WavpackContext* ctx_out = WavpackOpenFileOutput(
        [](void* id, void* data, int32_t bcount) -> int32_t {
            return static_cast<int32_t>(std::fwrite(data, 1, bcount, static_cast<FILE*>(id)));
        },
        out,
        error
    );

    if (!ctx_out) {
        WavpackCloseFile(ctx_in);
        std::fclose(in);
        std::fclose(out);
        Logger::log(LogLevel::ERROR, std::string("WavPack output open failed: ") + error, "wavpack_encoder");
        throw std::runtime_error("WavPack output open failed");
    }

    if (!WavpackSetConfiguration(ctx_out, &config, -1)) {
        WavpackCloseFile(ctx_in);
        WavpackCloseFile(ctx_out);
        std::fclose(in);
        std::fclose(out);
        Logger::log(LogLevel::ERROR, "Failed to set WavPack configuration", "wavpack_encoder");
        throw std::runtime_error("Failed to set WavPack configuration");
    }

    // copy samples
    const int32_t num_channels = config.num_channels;
    constexpr int32_t block_size = 4096;
    std::vector<int32_t> buffer(block_size * num_channels);

    int32_t samples;
    while ((samples = WavpackUnpackSamples(ctx_in, buffer.data(), block_size)) > 0) {
        if (!WavpackPackSamples(ctx_out, buffer.data(), samples)) {
            Logger::log(LogLevel::ERROR, "Error packing samples", "wavpack_encoder");
            WavpackCloseFile(ctx_in);
            WavpackCloseFile(ctx_out);
            std::fclose(in);
            std::fclose(out);
            throw std::runtime_error("Error packing samples");
        }
    }

    if (!WavpackFlushSamples(ctx_out)) {
        Logger::log(LogLevel::ERROR, "Error flushing samples", "wavpack_encoder");
        WavpackCloseFile(ctx_in);
        WavpackCloseFile(ctx_out);
        std::fclose(in);
        std::fclose(out);
        throw std::runtime_error("Error flushing samples");
    }

    // copy metadata if requested
    if (preserve_metadata_) {
        int num_tags = WavpackGetNumTagItems(ctx_in);
        for (int i = 0; i < num_tags; ++i) {
            char tag_name[256];
            if (WavpackGetTagItemIndexed(ctx_in, i, tag_name, sizeof(tag_name))) {
                int size = WavpackGetTagItem(ctx_in, tag_name, nullptr, 0);
                if (size > 0) {
                    std::vector<char> value(size + 1);
                    WavpackGetTagItem(ctx_in, tag_name, value.data(), size + 1);
                    WavpackAppendTagItem(ctx_out, tag_name, value.data(), size);
                }
            }
        }
        WavpackWriteTag(ctx_out);
    }

    WavpackCloseFile(ctx_in);
    WavpackCloseFile(ctx_out);
    std::fclose(in);
    std::fclose(out);

    Logger::log(LogLevel::INFO, "WavPack recompression completed: " + output.string(), "wavpack_encoder");
    return true;
}