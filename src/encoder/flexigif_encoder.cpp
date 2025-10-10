//
// Created by Giuseppe Francione on 10/10/25.
//

#include "flexigif_encoder.hpp"
#include "../utils/logger.hpp"
#include "GifImage.h"
#include "LzwEncoder.h"
#include <filesystem>
#include <stdexcept>

FlexiGifEncoder::FlexiGifEncoder(const bool preserve_metadata) {
    preserve_metadata_ = preserve_metadata;
}

bool FlexiGifEncoder::recompress(const std::filesystem::path &input,
                                 const std::filesystem::path &output) {
    // log start
    Logger::log(LogLevel::INFO,
                "Start flexiGIF recompression: " + input.string(),
                "flexigif_encoder");

    try {
        // if metadata should not be preserved, flexiGIF cannot strip headers/extensions.
        // but flexiGIF is supposed to be used after gifsicle, which will strip metadata anyway.

        // load gif
        GifImage gif(input.string());

        // validate decoded content
        const unsigned int numFrames = gif.getNumFrames();
        if (numFrames == 0) {
            Logger::log(LogLevel::ERROR,
                        "Decoded GIF has no frames; skipping: " + input.string(),
                        "flexigif_encoder");
            return false;
        }

        // optimize each frame with lzw encoder
        std::vector<std::vector<bool>> optimizedBits;
        optimizedBits.reserve(numFrames);

        for (unsigned int frameIndex = 0; frameIndex < numFrames; frameIndex++) {
            const auto& frame = gif.getFrame(frameIndex);
            const auto& indices = frame.pixels;

            if (indices.empty()) {
                Logger::log(LogLevel::WARNING,
                            "Empty GIF frame; skipping frame " + std::to_string(frameIndex + 1),
                            "flexigif_encoder");
                optimizedBits.emplace_back();
                continue;
            }

            LzwEncoder encoder(indices, /*isGif=*/true);

            LzwEncoder::OptimizationSettings settings{};
            settings.minCodeSize         = frame.codeSize;
            settings.startWithClearCode  = true;
            settings.verbose             = false;

            // balanced settings for reasonable speed and good compression
            settings.greedy              = true;
            settings.minNonGreedyMatch   = 2;
            settings.minImprovement      = 1;
            settings.maxDictionary       = 4096;
            settings.maxTokens           = 10000;
            settings.splitRuns           = false;
            settings.alignment           = 10;

            settings.readOnlyBest        = false;
            settings.avoidNonGreedyAgain = true;

            // pre-pass: estimate costs over aligned starts
            const int lastPos = static_cast<int>(indices.size()) - 1;
            for (int i = lastPos; i >= 0; i--) {
                if ((i % static_cast<int>(settings.alignment)) != 0)
                    continue;

                encoder.optimizePartial(static_cast<unsigned int>(i),
                                        0,
                                        false,
                                        true,
                                        settings);
            }

            // final bitstream along the shortest path
            settings.readOnlyBest = true;
            auto bits = encoder.optimize(settings);
            optimizedBits.push_back(std::move(bits));
        }

        gif.writeOptimized(output.string(), optimizedBits);

        Logger::log(LogLevel::INFO,
                    "flexiGIF recompression completed: " + output.string(),
                    "flexigif_encoder");
        return true;
    }
    catch (const char* e) {
        Logger::log(LogLevel::ERROR,
                    std::string("flexiGIF error: ") + (e ? e : "(no message)"),
                    "flexigif_encoder");
        return false;
    }
    catch (const std::exception& e) {
        Logger::log(LogLevel::ERROR,
                    std::string("flexiGIF error: ") + e.what(),
                    "flexigif_encoder");
        return false;
    }
    catch (...) {
        Logger::log(LogLevel::ERROR,
                    "Unknown flexiGIF error while processing " + input.string(),
                    "flexigif_encoder");
        return false;
    }
}