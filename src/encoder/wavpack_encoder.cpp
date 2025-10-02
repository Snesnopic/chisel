//
// Created by Giuseppe Francione on 25/09/25.
//

#include "wavpack_encoder.hpp"
#include "../utils/logger.hpp"
#include <wavpack.h>
#include <stdexcept>
#include <vector>
#include <cstdio>
#include <filesystem>

WavpackEncoder::WavpackEncoder(const bool preserve_metadata) {
    preserve_metadata_ = preserve_metadata;
}

// recompress a wavpack file into wavpack lossless with maximum compression
bool WavpackEncoder::recompress(const std::filesystem::path& input,
                                const std::filesystem::path& output) {
    Logger::log(LogLevel::INFO, "Starting WavPack recompression: " + input.string(), "wavpack_encoder");

    char error[128]{};

    // open input context
    WavpackContext* ctx_in = WavpackOpenFileInput(input.string().c_str(), error, OPEN_TAGS, 0);
    if (!ctx_in) {
        Logger::log(LogLevel::ERROR, std::string("WavPack open failed: ") + error, "wavpack_encoder");
        throw std::runtime_error("WavPack open failed");
    }

    // prepare output
    FILE* out = std::fopen(output.string().c_str(), "wb");
    if (!out) {
        WavpackCloseFile(ctx_in);
        Logger::log(LogLevel::ERROR, "Cannot open output file: " + output.string(), "wavpack_encoder");
        throw std::runtime_error("Cannot open output file");
    }

    // open output context
    WavpackContext* ctx_out = WavpackOpenFileOutput(
        [](void* id, void* data, int32_t bcount) -> int32_t {
            return static_cast<int32_t>(std::fwrite(data, 1, static_cast<size_t>(bcount), static_cast<FILE*>(id)));
        },
        out,
        error
    );

    if (!ctx_out) {
        std::fclose(out);
        WavpackCloseFile(ctx_in);
        Logger::log(LogLevel::ERROR, std::string("WavPack output open failed: ") + error, "wavpack_encoder");
        throw std::runtime_error("WavPack output open failed");
    }

    // configure encoder
    WavpackConfig config{};
    config.bytes_per_sample = WavpackGetBytesPerSample(ctx_in);
    config.bits_per_sample  = WavpackGetBitsPerSample(ctx_in);
    config.num_channels     = WavpackGetNumChannels(ctx_in);
    config.sample_rate      = WavpackGetSampleRate(ctx_in);
    config.qmode            = 0;                  // PCM int
    config.block_samples    = 0;                  // let encoder decide
    config.flags            = 0;

    config.flags |= CONFIG_VERY_HIGH_FLAG;

    config.flags &= ~CONFIG_HYBRID_FLAG;

    if (!WavpackSetConfiguration(ctx_out, &config, -1)) {
        WavpackCloseFile(ctx_out);
        std::fclose(out);
        WavpackCloseFile(ctx_in);
        Logger::log(LogLevel::ERROR, "Failed to set WavPack configuration", "wavpack_encoder");
        throw std::runtime_error("Failed to set WavPack configuration");
    }

    // initialize packing, otherwise the encoder is not ready and may misbehave
    if (!WavpackPackInit(ctx_out)) {
        WavpackCloseFile(ctx_out);
        std::fclose(out);
        WavpackCloseFile(ctx_in);
        Logger::log(LogLevel::ERROR, "WavpackPackInit failed", "wavpack_encoder");
        throw std::runtime_error("WavpackPackInit failed");
    }

    // copy samples: decoder provides 32-bit signed PCM in interleaved channels
    const int32_t num_channels = config.num_channels > 0 ? config.num_channels : 1;
    constexpr int32_t block_size = 65536; // larger blocks to reduce overhead
    std::vector<int32_t> buffer(static_cast<size_t>(block_size) * static_cast<size_t>(num_channels));

    uint32_t samples = 0;
    while ((samples = WavpackUnpackSamples(ctx_in, buffer.data(), block_size)) > 0) {
        if (!WavpackPackSamples(ctx_out, buffer.data(), static_cast<int32_t>(samples))) {
            Logger::log(LogLevel::ERROR, "Error packing samples", "wavpack_encoder");
            WavpackCloseFile(ctx_out);
            std::fclose(out);
            WavpackCloseFile(ctx_in);
            throw std::runtime_error("Error packing samples");
        }
    }

    if (!WavpackFlushSamples(ctx_out)) {
        Logger::log(LogLevel::ERROR, "Error flushing samples", "wavpack_encoder");
        WavpackCloseFile(ctx_out);
        std::fclose(out);
        WavpackCloseFile(ctx_in);
        throw std::runtime_error("Error flushing samples");
    }

    // copy metadata after samples have been written
    if (preserve_metadata_) {
        const int num_tags = WavpackGetNumTagItems(ctx_in);
        for (int i = 0; i < num_tags; ++i) {
            char tag_name[256];
            if (WavpackGetTagItemIndexed(ctx_in, i, tag_name, sizeof(tag_name))) {
                int size = WavpackGetTagItem(ctx_in, tag_name, nullptr, 0);
                if (size > 0) {
                    std::vector<char> value(static_cast<size_t>(size) + 1);
                    if (WavpackGetTagItem(ctx_in, tag_name, value.data(), size + 1) > 0) {
                        if (!WavpackAppendTagItem(ctx_out, tag_name, value.data(), size)) {
                            Logger::log(LogLevel::WARNING,
                                        std::string("Failed to append tag: ") + tag_name,
                                        "wavpack_encoder");
                        }
                    }
                }
            }
        }
        if (!WavpackWriteTag(ctx_out)) {
            Logger::log(LogLevel::WARNING, "Failed to write tags", "wavpack_encoder");
        }
    }

    // close contexts and output
    WavpackCloseFile(ctx_out);
    std::fclose(out);
    WavpackCloseFile(ctx_in);

    Logger::log(LogLevel::INFO, "WavPack recompression completed: " + output.string(), "wavpack_encoder");
    return true;
}