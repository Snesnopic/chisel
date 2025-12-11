//
// Created by Giuseppe Francione on 19/10/25.
//

#include "../../include/gif_processor.hpp"
#include "../../include/logger.hpp"
#include "file_utils.hpp"
#include "stb_image.h"
#include <filesystem>
#include <cstdio>
#include <stdexcept>
#include <cstring>
#include <thread>
#include <exception>
#include <vector>

extern "C" {
#include <lcdfgif/gif.h>
#include "gifsicle.h"
}

namespace chisel {

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
    size_t read_count = fread(buf.data(), 1, size, f);
    fclose(f);

    if (read_count != static_cast<size_t>(size)) {
        return {};
    }
    return buf;
}

void GifProcessor::recompress(const std::filesystem::path& input,
                              const std::filesystem::path& output,
                              const bool preserve_metadata) {

    std::exception_ptr error_ptr = nullptr;

    std::thread worker([&]() {
        try {
            Logger::log(LogLevel::Info, "Start GIF recompression: " + input.string(), "gif_processor");

            FILE* in = chisel::open_file(input, "rb");
            if (!in) throw std::runtime_error("Cannot open GIF input");

            Gif_Stream* gfs = Gif_ReadFile(in);
            std::fclose(in);

            if (!gfs) throw std::runtime_error("Failed to read GIF structure");

            if (!preserve_metadata) {
                if (gfs->end_comment) {
                    Gif_DeleteComment(gfs->end_comment);
                    gfs->end_comment = nullptr;
                }
                for (int i = 0; i < gfs->nimages; ++i) {
                    if (gfs->images[i]->comment) {
                        Gif_DeleteComment(gfs->images[i]->comment);
                        gfs->images[i]->comment = nullptr;
                    }
                }
            }

            FILE* out = chisel::open_file(output, "wb");
            if (!out) {
                Gif_DeleteStream(gfs);
                throw std::runtime_error("Cannot open GIF output");
            }

            Gif_CompressInfo local_info;
            std::memset(&local_info, 0, sizeof(local_info));
            Gif_InitCompressInfo(&local_info);

            if (!Gif_FullWriteFile(gfs, &local_info, out)) {
                std::fclose(out);
                Gif_DeleteStream(gfs);
                throw std::runtime_error("Gif_FullWriteFile failed");
            }

            std::fclose(out);

            Gif_DeleteStream(gfs);

        } catch (...) {
            error_ptr = std::current_exception();
        }
    });

    worker.join();
    if (error_ptr) {
        std::rethrow_exception(error_ptr);
    }
}

bool GifProcessor::raw_equal(const std::filesystem::path& a, const std::filesystem::path& b) const {
    const auto bufA = read_file_to_buffer(a);
    const auto bufB = read_file_to_buffer(b);

    if (bufA.empty() || bufB.empty()) {
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
        equal = false;
    } else {
        size_t totalBytes = static_cast<size_t>(wA) * hA * 4 * framesA;
        if (std::memcmp(dataA, dataB, totalBytes) != 0) {
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