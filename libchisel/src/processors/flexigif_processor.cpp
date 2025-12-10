//
// Created by Giuseppe Francione on 19/10/25.
//

#include "../../include/flexigif_processor.hpp"
#include "../../include/logger.hpp"
#include "GifImage.h"
#include "LzwEncoder.h"
#include <stdexcept>
#include <vector>
#include "file_utils.hpp"
#include "stb_image.h"
#include <cstring>
namespace chisel {

void FlexiGifProcessor::recompress(const std::filesystem::path& input,
                                   const std::filesystem::path& output,
                                   bool /*preserve_metadata*/) {
    Logger::log(LogLevel::Info,
                "Start flexiGIF recompression: " + input.string(),
                "flexigif_processor");

    try {
        GifImage gif(input.string());

        const unsigned int numFrames = gif.getNumFrames();
        if (numFrames == 0) {
            Logger::log(LogLevel::Error,
                        "Decoded GIF has no frames; skipping: " + input.string(),
                        "flexigif_processor");
            return;
        }

        std::vector<std::vector<bool>> optimizedBits;
        optimizedBits.reserve(numFrames);

        for (unsigned int frameIndex = 0; frameIndex < numFrames; frameIndex++) {
            const auto& frame = gif.getFrame(frameIndex);
            const auto& indices = frame.pixels;

            if (indices.empty()) {
                Logger::log(LogLevel::Warning,
                            "Empty GIF frame; skipping frame " + std::to_string(frameIndex + 1),
                            "flexigif_processor");
                optimizedBits.emplace_back();
                continue;
            }

            LzwEncoder encoder(indices, /*isGif=*/true);

            LzwEncoder::OptimizationSettings settings{};
            settings.minCodeSize         = frame.codeSize;
            settings.startWithClearCode  = true;
            settings.verbose             = false;
            settings.greedy              = true;
            settings.minNonGreedyMatch   = 2;
            settings.minImprovement      = 1;
            settings.maxDictionary       = 4096;
            settings.maxTokens           = 10000;
            settings.splitRuns           = false;
            settings.alignment           = 10;
            settings.readOnlyBest        = false;
            settings.avoidNonGreedyAgain = true;

            // pre-pass
            const int lastPos = static_cast<int>(indices.size()) - 1;
            for (int i = lastPos; i >= 0; i--) {
                if ((i % static_cast<int>(settings.alignment)) != 0) continue;
                encoder.optimizePartial(static_cast<unsigned int>(i),
                                        0,
                                        false,
                                        true,
                                        settings);
            }

            // final pass
            settings.readOnlyBest = true;
            auto bits = encoder.optimize(settings);
            optimizedBits.push_back(std::move(bits));
        }

        gif.writeOptimized(output.string(), optimizedBits);

        Logger::log(LogLevel::Info,
                    "flexiGIF recompression completed: " + output.string(),
                    "flexigif_processor");
    }
    catch (const std::exception& e) {
        Logger::log(LogLevel::Error,
                    std::string("flexiGIF error: ") + e.what(),
                    "flexigif_processor");
        throw;
    }
    catch (...) {
        Logger::log(LogLevel::Error,
                    "Unknown flexiGIF error while processing " + input.string(),
                    "flexigif_processor");
        throw;
    }
}

static std::vector<unsigned char> read_file_to_buffer(const std::filesystem::path& path) {
    FILE* f = chisel::open_file(path, "rb");
    if (!f) return {};
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        fclose(f);
        return {};
    }
    std::vector<unsigned char> buf(size);
    fread(buf.data(), 1, size, f);
    fclose(f);
    return buf;
}

bool FlexiGifProcessor::raw_equal(const std::filesystem::path& a, const std::filesystem::path& b) const {
    auto bufA = read_file_to_buffer(a);
    auto bufB = read_file_to_buffer(b);

    if (bufA.empty() || bufB.empty()) {
        Logger::log(LogLevel::Warning, "raw_equal: empty or unreadable file(s)", "flexigif_processor");
        return false;
    }

    int wA, hA, framesA;
    int* delaysA = nullptr;

    unsigned char* dataA = stbi_load_gif_from_memory(
        bufA.data(), static_cast<int>(bufA.size()),
        &delaysA, &wA, &hA, &framesA, nullptr, 4
    );

    if (!dataA) {
        Logger::log(LogLevel::Warning, "raw_equal: failed to decode GIF A", "flexigif_processor");
        return false;
    }

    int wB, hB, framesB;
    int* delaysB = nullptr;
    unsigned char* dataB = stbi_load_gif_from_memory(
        bufB.data(), static_cast<int>(bufB.size()),
        &delaysB, &wB, &hB, &framesB, nullptr, 4
    );

    if (!dataB) {
        Logger::log(LogLevel::Warning, "raw_equal: failed to decode GIF B", "flexigif_processor");
        stbi_image_free(dataA);
        if (delaysA) stbi_image_free(delaysA);
        return false;
    }

    bool equal = true;

    if (wA != wB || hA != hB || framesA != framesB) {
        Logger::log(LogLevel::Debug, "raw_equal: dimension/frame count mismatch", "flexigif_processor");
        equal = false;
    } else {
        size_t totalBytes = static_cast<size_t>(wA) * hA * 4 * framesA;
        if (std::memcmp(dataA, dataB, totalBytes) != 0) {
            Logger::log(LogLevel::Debug, "raw_equal: pixel mismatch", "flexigif_processor");
            equal = false;
        }
    }

    stbi_image_free(dataA);
    if (delaysA) stbi_image_free(delaysA);
    stbi_image_free(dataB);
    if (delaysB) stbi_image_free(delaysB);

    return equal;
}

std::string FlexiGifProcessor::get_raw_checksum(const std::filesystem::path& file_path) const {
    // TODO: implement checksum of raw GIF data
    return "";
}

} // namespace chisel