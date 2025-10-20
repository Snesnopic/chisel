//
// Created by Giuseppe Francione on 19/10/25.
//

#include "../../include/flexigif_processor.hpp"
#include "../../include/logger.hpp"
#include "GifImage.h"
#include "LzwEncoder.h"
#include <stdexcept>
#include <vector>

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

std::string FlexiGifProcessor::get_raw_checksum(const std::filesystem::path& file_path) const {
    // TODO: implement checksum of raw GIF data
    return "";
}

} // namespace chisel