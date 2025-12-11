//
// Created by Giuseppe Francione on 19/10/25.
//

#include "../../include/gif_processor.hpp"
#include "../../include/logger.hpp"
#include <filesystem>
#include <cstdio>
#include <stdexcept>
#include <mutex>
#include "file_utils.hpp"
#include "stb_image.h"
extern "C" {
#include <lcdfgif/gif.h>
#include "gifsicle.h"
}

namespace chisel {


void GifProcessor::recompress(const std::filesystem::path& input,
                              const std::filesystem::path& output,
                              bool preserve_metadata) {
    Logger::log(LogLevel::Info,
                "Start GIF recompression: " + input.string(),
                "gif_processor");
    FILE* in = chisel::open_file(input.string().c_str(), "rb");
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

    optimize_fragments(gfs, 3, 0);

    FILE* out = chisel::open_file(output.string().c_str(), "wb");
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

bool GifProcessor::raw_equal(const std::filesystem::path& a, const std::filesystem::path& b) const {
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
        Logger::log(LogLevel::Warning, "raw_equal: failed to decode GIF A", "gif_processor");
        return false;
    }

    int wB, hB, framesB;
    int* delaysB = nullptr;
    unsigned char* dataB = stbi_load_gif_from_memory(
        bufB.data(), static_cast<int>(bufB.size()),
        &delaysB, &wB, &hB, &framesB, nullptr, 4
    );

    if (!dataB) {
        Logger::log(LogLevel::Warning, "raw_equal: failed to decode GIF B", "gif_processor");
        stbi_image_free(dataA);
        if (delaysA) stbi_image_free(delaysA);
        return false;
    }

    bool equal = true;

    if (wA != wB || hA != hB || framesA != framesB) {
        Logger::log(LogLevel::Debug, "raw_equal: dimension/frame count mismatch", "gif_processor");
        equal = false;
    } else {
        size_t totalBytes = static_cast<size_t>(wA) * hA * 4 * framesA;
        if (std::memcmp(dataA, dataB, totalBytes) != 0) {
            Logger::log(LogLevel::Debug, "raw_equal: pixel mismatch", "gif_processor");
            equal = false;
        }
    }

    stbi_image_free(dataA);
    if (delaysA) stbi_image_free(delaysA);
    stbi_image_free(dataB);
    if (delaysB) stbi_image_free(delaysB);

    return equal;
}

std::string GifProcessor::get_raw_checksum(const std::filesystem::path&) const {
    // TODO: implement checksum of raw GIF data
    return "";
}

} // namespace chisel