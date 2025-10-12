//
// Created by Giuseppe Francione on 07/10/25.
//

extern "C" {
#include <lcdfgif/gif.h>
#include "gifsicle.h"
}
#include "gif_encoder.hpp"
#include "../utils/logger.hpp"
#include <filesystem>
#include <cstdio>
#include <stdexcept>
#include <cstring>
#include <atomic>
#include <mutex>

// global mutex for optimize_fragments
static std::mutex gifsicle_mutex;

// global compress info required by gifsicle
Gif_CompressInfo gif_write_info;

GifEncoder::GifEncoder(const bool preserve_metadata) {
    preserve_metadata_ = preserve_metadata;
}

bool GifEncoder::recompress(const std::filesystem::path &input,
                            const std::filesystem::path &output) {
    Logger::log(LogLevel::Info,
                "Start GIF recompression: " + input.string(),
                "gif_encoder");

    FILE* in = std::fopen(input.string().c_str(), "rb");
    if (!in) {
        Logger::log(LogLevel::Error,
                    "Cannot open GIF input: " + input.string(),
                    "gif_encoder");
        throw std::runtime_error("Cannot open GIF input");
    }

    Gif_Stream* gfs = Gif_ReadFile(in);
    std::fclose(in);

    if (!gfs) {
        Logger::log(LogLevel::Error,
                    "Failed to read GIF: " + input.string(),
                    "gif_encoder");
        return false;
    }

    if (!preserve_metadata_) {
        if (gfs->end_comment) {
            Gif_DeleteComment(gfs->end_comment);
            gfs->end_comment = nullptr;
        }
        if (gfs->end_extension_list) {
            Gif_DeleteExtension(gfs->end_extension_list);
            gfs->end_extension_list = nullptr;
        }
    }

    // optimize fragments with gifsicle (protected by mutex, because gifsicle uses global variables)
    {
        std::lock_guard<std::mutex> lock(gifsicle_mutex);
        optimize_fragments(gfs, 3, 0);
    }

    FILE* out = std::fopen(output.string().c_str(), "wb");
    if (!out) {
        Logger::log(LogLevel::Error,
                    "Cannot open GIF output: " + output.string(),
                    "gif_encoder");
        Gif_DeleteStream(gfs);
        throw std::runtime_error("Cannot open GIF output");
    }

    // local compress info
    Gif_CompressInfo local_info;
    Gif_InitCompressInfo(&local_info);

    if (!Gif_FullWriteFile(gfs, &local_info, out)) {
        Logger::log(LogLevel::Error,
                    "Failed to write GIF: " + output.string(),
                    "gif_encoder");
        std::fclose(out);
        Gif_DeleteStream(gfs);
        return false;
    }

    std::fclose(out);
    Gif_DeleteStream(gfs);

    Logger::log(LogLevel::Info,
                "GIF recompression completed: " + output.string(),
                "gif_encoder");
    return true;
}