//
// Created by Giuseppe Francione on 19/10/25.
//

#include "../../include/wavpack_processor.hpp"
#include "../../include/logger.hpp"
#include <wavpack.h>
#include <stdexcept>
#include <vector>
#include <cstdio>
#include <filesystem>
#include "file_utils.hpp"


namespace chisel {

namespace {
// .wvc correction files share the same "wvpk" block signature as regular .wv files
// (so the MIME sniffer and, in turn, the executor can dispatch one directly here on
// its own), but they can never be unpacked standalone: their content is only
// meaningful alongside their .wv counterpart, already merged in via OPEN_WVC below.
// libwavpack's own error message for this case isn't reliable to pattern-match on
// (it varies depending on the correction file's internal structure/size), so detect
// this purely from the extension instead.
bool is_correction_file(const std::filesystem::path& p) {
    auto ext = p.extension().string();
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".wvc";
}
}

void WavPackProcessor::recompress(const std::filesystem::path& input,
                                  const std::filesystem::path& output, const ProcessingOptions &options) {
    Logger::log(LogLevel::Debug, "Entering recompress for " + input.string(), get_name());

    if (is_correction_file(input)) {
        Logger::log(LogLevel::Debug, "Skipping standalone WavPack correction file: " + input.string(), get_name());
        std::vector<uint8_t> data;
        if (!read_file(input, data) || !write_file(output, data))
            throw std::runtime_error("WavPack: failed to pass through correction file");
        return;
    }

    char error[128]{};

    // open input context
    // OPEN_WVC: auto-load a sibling .wvc correction file, without which a
    // hybrid-mode source would be silently decoded as its lossy-only approximation
    WavpackContext* ctx_in = WavpackOpenFileInput(input.string().c_str(), error, OPEN_TAGS | OPEN_WVC, 0);
    if (!ctx_in) {
        Logger::log(LogLevel::Error, std::string("Wavpack open failed: ") + error, get_name());
        throw std::runtime_error("WavPack open failed");
    }

    // prepare output
    FILE* out = chisel::open_file(output.string().c_str(), "wb");
    if (!out) {
        WavpackCloseFile(ctx_in);
        throw std::runtime_error("Cannot open output file");
    }

    // open output context
    WavpackContext* ctx_out = WavpackOpenFileOutput(
        [](void* id, void* data, const int32_t bcount) -> int32_t {
            return static_cast<int32_t>(std::fwrite(data, 1, static_cast<std::size_t>(bcount), static_cast<FILE*>(id)));
        },
        out,
        nullptr
    );

    if (!ctx_out) {
        std::fclose(out);
        WavpackCloseFile(ctx_in);
        throw std::runtime_error(std::string("WavPack output open failed: ") + error);
    }

    // configure encoder
    WavpackConfig config{};
    config.bytes_per_sample = WavpackGetBytesPerSample(ctx_in);
    config.bits_per_sample  = WavpackGetBitsPerSample(ctx_in);
    config.num_channels     = WavpackGetNumChannels(ctx_in);
    config.sample_rate      = static_cast<int32_t>(WavpackGetSampleRate(ctx_in));
    config.qmode            = 0;
    config.block_samples    = 0;
    config.flags            = CONFIG_VERY_HIGH_FLAG | CONFIG_EXTRA_MODE;
    config.xmode            = 6;
    config.flags &= ~CONFIG_HYBRID_FLAG;             // force lossless

    if (!WavpackSetConfiguration(ctx_out, &config, -1)) {
        WavpackCloseFile(ctx_out);
        std::fclose(out);
        WavpackCloseFile(ctx_in);
        throw std::runtime_error("Failed to set WavPack configuration");
    }

    if (!WavpackPackInit(ctx_out)) {
        WavpackCloseFile(ctx_out);
        std::fclose(out);
        WavpackCloseFile(ctx_in);
        throw std::runtime_error("WavpackPackInit failed");
    }

    const int32_t num_channels = config.num_channels > 0 ? config.num_channels : 1;
    constexpr int32_t block_size = 65536;
    std::vector<int32_t> buffer(static_cast<std::size_t>(block_size) * static_cast<std::size_t>(num_channels));

    uint32_t samples = 0;
    while ((samples = WavpackUnpackSamples(ctx_in, buffer.data(), block_size)) > 0) {
        if (!WavpackPackSamples(ctx_out, buffer.data(), static_cast<int32_t>(samples))) {
            WavpackCloseFile(ctx_out);
            std::fclose(out);
            WavpackCloseFile(ctx_in);
            throw std::runtime_error("Error packing samples");
        }
    }

    if (!WavpackFlushSamples(ctx_out)) {
        WavpackCloseFile(ctx_out);
        std::fclose(out);
        WavpackCloseFile(ctx_in);
        throw std::runtime_error("Error flushing samples");
    }

    // copy metadata
    if (options.preserve_metadata) {
        const int num_tags = WavpackGetNumTagItems(ctx_in);
        for (int i = 0; i < num_tags; ++i) {
            char tag_name[256];
            if (WavpackGetTagItemIndexed(ctx_in, i, tag_name, sizeof(tag_name))) {
                const int size = WavpackGetTagItem(ctx_in, tag_name, nullptr, 0);
                if (size > 0) {
                    std::vector<char> value(static_cast<std::size_t>(size) + 1);
                    if (WavpackGetTagItem(ctx_in, tag_name, value.data(), size + 1) > 0) {
                        if (!WavpackAppendTagItem(ctx_out, tag_name, value.data(), size)) {
                            Logger::log(LogLevel::Warning,
                                        std::string("Failed to append tag: ") + tag_name,
                                        get_name());
                        }
                    }
                }
            }
        }
        if (!WavpackWriteTag(ctx_out)) {
            Logger::log(LogLevel::Warning, "Failed to write tags", get_name());
        }
    }

    WavpackCloseFile(ctx_out);
    std::fclose(out);
    WavpackCloseFile(ctx_in);

    Logger::log(LogLevel::Debug, "Exiting recompress for " + output.string(), get_name());
}

std::string WavPackProcessor::get_raw_checksum(const std::filesystem::path&) const {
    // TODO: implement checksum of raw WavPack data
    return "";
}

/**
 * @brief Decodes a WavPack file into a raw PCM audio buffer.
 * @param file The path to the WavPack file.
 * @param sample_rate Output parameter for the sample rate.
 * @param channels Output parameter for the number of channels.
 * @param bps Output parameter for the bits per sample.
 * @return A vector of 32-bit integers representing the decoded PCM data.
 */
std::vector<int32_t> decode_wavpack_pcm(const std::filesystem::path& file,
                                        int& sample_rate,
                                        int& channels,
                                        int& bps) {
    char error[128]{};
    WavpackContext* ctx = WavpackOpenFileInput(file.string().c_str(), error, OPEN_TAGS | OPEN_WVC, 0);
    if (!ctx) {
        throw std::runtime_error("WavPack open failed: " + std::string(error));
    }

    sample_rate = static_cast<int>(WavpackGetSampleRate(ctx));
    channels    = WavpackGetNumChannels(ctx);
    bps         = WavpackGetBitsPerSample(ctx);

    if (channels <= 0 || sample_rate <= 0 || bps <= 0) {
        WavpackCloseFile(ctx);
        throw std::runtime_error("Invalid WavPack parameters");
    }

    const int32_t num_channels = channels;
    constexpr int32_t block_size = 65536;
    std::vector<int32_t> buffer(static_cast<std::size_t>(block_size) * static_cast<std::size_t>(num_channels));
    std::vector<int32_t> pcm;

    uint32_t samples = 0;
    while ((samples = WavpackUnpackSamples(ctx, buffer.data(), block_size)) > 0) {
        pcm.insert(pcm.end(), buffer.begin(), buffer.begin() + samples * num_channels);
    }

    WavpackCloseFile(ctx);
    return pcm;
}


bool WavPackProcessor::raw_equal(const std::filesystem::path& a,
                                 const std::filesystem::path& b) const {
    // standalone .wvc correction files (passed through unchanged by recompress())
    // can't be decoded to PCM on their own; fall back to a byte compare
    if (is_correction_file(a) || is_correction_file(b)) {
        std::vector<uint8_t> bufA, bufB;
        if (!read_file(a, bufA) || !read_file(b, bufB)) return false;
        return bufA == bufB;
    }

    try {
        int ra, ca, bpsa;
        int rb, cb, bpsb;
        const auto pcmA = decode_wavpack_pcm(a, ra, ca, bpsa);
        const auto pcmB = decode_wavpack_pcm(b, rb, cb, bpsb);

        if (ra != rb || ca != cb || bpsa != bpsb) return false;
        return pcmA == pcmB;
    } catch (const std::exception&) {
        return false;
    }
}

} // namespace chisel