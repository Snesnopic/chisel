//
// Created by Giuseppe Francione on 07/10/25.
//

#include "gif_encoder.hpp"
#include "../utils/logger.hpp"

extern "C" {
#include <lcdfgif/gif.h>
#include "gifsicle.h"
}

#include <filesystem>
#include <cstdio>
#include <stdexcept>

Gif_CompressInfo gif_write_info;

GifEncoder::GifEncoder(bool preserve_metadata) {
    preserve_metadata_ = preserve_metadata;
}

bool GifEncoder::recompress(const std::filesystem::path &input,
                            const std::filesystem::path &output) {
    Logger::log(LogLevel::INFO,
                "Start GIF recompression: " + input.string(),
                "gif_encoder");

    FILE* in = std::fopen(input.string().c_str(), "rb");
    if (!in) {
        Logger::log(LogLevel::ERROR,
                    "Cannot open GIF input: " + input.string(),
                    "gif_encoder");
        throw std::runtime_error("Cannot open GIF input");
    }

    Gif_Stream* gfs = Gif_ReadFile(in);
    std::fclose(in);

    if (!gfs) {
        Logger::log(LogLevel::ERROR,
                    "Failed to read GIF: " + input.string(),
                    "gif_encoder");
        return false;
    }

    if (!preserve_metadata_) {
        if (gfs->end_comment) {
            Gif_DeleteComment(gfs->end_comment);
            gfs->end_comment = nullptr;
        }
        while (gfs->end_extension_list) {
            Gif_DeleteExtension(gfs->end_extension_list);
        }
    }

    optimize_fragments(gfs, 3 /* livello */, 0 /* huge_stream */);

    FILE* out = std::fopen(output.string().c_str(), "wb");
    if (!out) {
        Logger::log(LogLevel::ERROR,
                    "Cannot open GIF output: " + output.string(),
                    "gif_encoder");
        Gif_DeleteStream(gfs);
        throw std::runtime_error("Cannot open GIF output");
    }

    if (!Gif_WriteFile(gfs, out)) {
        Logger::log(LogLevel::ERROR,
                    "Failed to write GIF: " + output.string(),
                    "gif_encoder");
        std::fclose(out);
        Gif_DeleteStream(gfs);
        return false;
    }

    std::fclose(out);
    Gif_DeleteStream(gfs);

    Logger::log(LogLevel::INFO,
                "GIF recompression completed: " + output.string(),
                "gif_encoder");
    return true;
}