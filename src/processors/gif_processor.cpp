//
// Created by Giuseppe Francione on 19/10/25.
//

#ifndef _WIN32
extern "C" {
#include <lcdfgif/gif.h>
#include "gifsicle.h"
}
#endif

#include "gif_processor.hpp"
#include <filesystem>
#include <cstdio>
#include <stdexcept>
#include <mutex>

namespace chisel {

#ifndef _WIN32
// global mutex for optimize_fragments (gifsicle uses global state)
static std::mutex gifsicle_mutex;
#endif

void GifProcessor::recompress(const std::filesystem::path& input,
                              const std::filesystem::path& output,
                              bool preserve_metadata) {
    Logger::log(LogLevel::Info,
                "Start GIF recompression: " + input.string(),
                "gif_processor");

#ifndef _WIN32
    FILE* in = std::fopen(input.string().c_str(), "rb");
    if (!in) {
        Logger::log(LogLevel::Error,
                    "Cannot open GIF input: " + input.string(),
                    "gif_processor");
        throw std::runtime_error("Cannot open GIF input");
    }

    Gif_Stream* gfs = Gif_ReadFile(in);
    std::fclose(in);

    if (!gfs) {
        Logger::log(LogLevel::Error,
                    "Failed to read GIF: " + input.string(),
                    "gif_processor");
        throw std::runtime_error("Failed to read GIF");
    }

    if (!preserve_metadata) {
        if (gfs->end_comment) {
            Gif_DeleteComment(gfs->end_comment);
            gfs->end_comment = nullptr;
        }
        if (gfs->end_extension_list) {
            Gif_DeleteExtension(gfs->end_extension_list);
            gfs->end_extension_list = nullptr;
        }
    }

    {
        std::lock_guard<std::mutex> lock(gifsicle_mutex);
        optimize_fragments(gfs, 3, 0);
    }

    FILE* out = std::fopen(output.string().c_str(), "wb");
    if (!out) {
        Logger::log(LogLevel::Error,
                    "Cannot open GIF output: " + output.string(),
                    "gif_processor");
        Gif_DeleteStream(gfs);
        throw std::runtime_error("Cannot open GIF output");
    }

    Gif_CompressInfo local_info;
    Gif_InitCompressInfo(&local_info);

    if (!Gif_FullWriteFile(gfs, &local_info, out)) {
        Logger::log(LogLevel::Error,
                    "Failed to write GIF: " + output.string(),
                    "gif_processor");
        std::fclose(out);
        Gif_DeleteStream(gfs);
        throw std::runtime_error("Failed to write GIF");
    }

    std::fclose(out);
    Gif_DeleteStream(gfs);

    Logger::log(LogLevel::Info,
                "GIF recompression completed: " + output.string(),
                "gif_processor");
#else
    // On Windows, gifsicle API is not available in this build configuration.
    // Keep interface consistent and fail explicitly.
    Logger::log(LogLevel::Error,
                "GIF recompression not supported on Windows in current build",
                "gif_processor");
    throw std::runtime_error("GIF recompression unsupported on Windows");
#endif
}

std::string GifProcessor::get_raw_checksum(const std::filesystem::path&) const {
    // TODO: implement checksum of raw GIF data
    return "";
}

} // namespace chisel