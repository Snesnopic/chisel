//
// Created by Giuseppe Francione on 12/11/25.
//

#include "../../include/tga_processor.hpp"
#include "../../include/logger.hpp"
#include <stdexcept>
#include <memory>
#include <vector>
#include <mutex>
#include <optional>
#include <cstring>
#include <fstream>
#include <algorithm>

// stb_image / stb_image_write's implementation lives in stb_image_impl.cpp,
// the one translation unit that defines it for the whole library
#include "../../third_party/stb/stb_image.h"
#include "file_utils.hpp"
#include "../../third_party/stb/stb_image_write.h"

namespace chisel {

namespace {
    void stbi_write_callback(void *context, void *data, const int size) {
        if (size <= 0) return;
        FILE* f = static_cast<FILE *>(context);
        std::fwrite(data, 1, static_cast<std::size_t>(size), f);
    }

    // stb_image_write's TGA writer never emits a TGA 2.0 footer/extension area (author,
    // comments, gamma, etc.), so it's pulled from the raw input bytes here instead
    struct TgaTrailer {
        std::vector<uint8_t> blob; // developer area + extension area, as one opaque region
        uint32_t ext_area_offset = 0;
        uint32_t dev_area_offset = 0;
        uint32_t blob_start = 0;
    };

    std::optional<TgaTrailer> extract_tga_trailer(const std::filesystem::path& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) return std::nullopt;
        f.seekg(0, std::ios::end);
        const auto file_size = static_cast<uint64_t>(f.tellg());
        if (file_size < 26) return std::nullopt;

        f.seekg(static_cast<std::streamoff>(file_size - 26));
        uint8_t footer[26];
        f.read(reinterpret_cast<char*>(footer), 26);
        if (!f || std::memcmp(footer + 8, "TRUEVISION-XFILE.", 17) != 0) return std::nullopt;

        const uint32_t ext_off = read_le32(footer);
        const uint32_t dev_off = read_le32(footer + 4);
        if (ext_off == 0 && dev_off == 0) return std::nullopt;

        const uint32_t blob_start = (ext_off != 0 && dev_off != 0) ? std::min(ext_off, dev_off)
                                                                    : (ext_off != 0 ? ext_off : dev_off);
        if (blob_start >= file_size - 26) return std::nullopt;

        TgaTrailer t;
        t.ext_area_offset = ext_off;
        t.dev_area_offset = dev_off;
        t.blob_start = blob_start;
        t.blob.resize(file_size - 26 - blob_start);
        f.seekg(blob_start);
        f.read(reinterpret_cast<char*>(t.blob.data()), static_cast<std::streamsize>(t.blob.size()));
        if (!f) return std::nullopt;

        return t;
    }
} // namespace

    void TgaProcessor::recompress(const std::filesystem::path& input,
                                  const std::filesystem::path& output, const ProcessingOptions &options) {

        Logger::log(LogLevel::Debug, "Entering recompress for " + input.string(), get_name());

        int width, height, channels;
        unique_FILE in_file(chisel::open_file(input, "rb"));
        if (!in_file) {
            Logger::log(LogLevel::Error, "Failed to open input file", get_name());
            throw std::runtime_error("TgaProcessor: Cannot open input");
        }
        // load the image
        unsigned char* data = stbi_load_from_file(in_file.get(), &width, &height, &channels, 0);
        in_file.reset();
        if (!data) {
            Logger::log(LogLevel::Error, std::string("Failed to load tga: ") + stbi_failure_reason(), get_name());
            throw std::runtime_error("TgaProcessor: Failed to load TGA");
        }

        const unique_FILE out_file(chisel::open_file(output, "wb"));
        if (!out_file) {
            stbi_image_free(data);
            Logger::log(LogLevel::Error, "Failed to open output file", get_name());
            throw std::runtime_error("TgaProcessor: Cannot open output");
        }

        // enable rle compression
        // TODO: fork?
        static std::mutex stb_tga_mtx;
        std::scoped_lock lock(stb_tga_mtx);
        stbi_write_tga_with_rle = 1;

        const int success = stbi_write_tga_to_func(
            stbi_write_callback,
            out_file.get(),
            width, height, channels, data
        );

        // free the image data
        stbi_image_free(data);

        if (!success) {
            Logger::log(LogLevel::Error, "Failed to write rle tga: " + output.string(), get_name());
            throw std::runtime_error("TgaProcessor: Failed to write TGA");
        }

        if (options.preserve_metadata) {
            if (auto trailer = extract_tga_trailer(input)) {
                const auto new_blob_start = static_cast<uint32_t>(std::ftell(out_file.get()));
                std::fwrite(trailer->blob.data(), 1, trailer->blob.size(), out_file.get());

                const auto shift = static_cast<int64_t>(new_blob_start) - static_cast<int64_t>(trailer->blob_start);
                const auto shifted = [shift](uint32_t off) {
                    return off ? static_cast<uint32_t>(static_cast<int64_t>(off) + shift) : 0;
                };

                uint8_t footer[26];
                write_le32(footer, shifted(trailer->ext_area_offset));
                write_le32(footer + 4, shifted(trailer->dev_area_offset));
                std::memcpy(footer + 8, "TRUEVISION-XFILE.", 17);
                footer[25] = 0;
                std::fwrite(footer, 1, sizeof(footer), out_file.get());
            }
        }

        Logger::log(LogLevel::Debug, "Exiting recompress for " + output.string(), get_name());
    }

    std::string TgaProcessor::get_raw_checksum(const std::filesystem::path& /*file_path*/) const {
        // not implemented for tga
        return "";
    }
    // helper to load a tga image into a raw rgba8 buffer
static std::vector<unsigned char> decode_tga_rgba8(const std::filesystem::path& file,
                                                   int& width,
                                                   int& height,
                                                   int& channels) {
    // force 4 channels (rgba) for consistent comparison
    const unique_FILE in_file(chisel::open_file(file, "rb"));
    if (!in_file) {
        Logger::log(LogLevel::Warning, "Raw_equal: Failed to open tga: " + file.string(), "TgaProcessor");
        return {};
    }
    unsigned char* data = stbi_load_from_file(in_file.get(), &width, &height, &channels, 4);
    if (!data) {
        Logger::log(LogLevel::Warning, std::string("Raw_equal: Failed to load tga: ") + stbi_failure_reason(), "TgaProcessor");
        return {};
    }

    channels = 4;
    const size_t data_size = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;

    // copy data to vector for safe handling
    std::vector<unsigned char> pcm(data, data + data_size);

    // free stb memory
    stbi_image_free(data);

    return pcm;
}

bool TgaProcessor::raw_equal(const std::filesystem::path &a,
                             const std::filesystem::path &b) const {
    int wa, ha, ca;
    int wb, hb, cb;

    std::vector<unsigned char> pcmA;
    std::vector<unsigned char> pcmB;

    try {
        pcmA = decode_tga_rgba8(a, wa, ha, ca);
    } catch (const std::exception& e) {
        Logger::log(LogLevel::Error, "Raw_equal: Error decoding " + a.string() + ": " + e.what(), get_name());
        return false;
    }

    try {
        pcmB = decode_tga_rgba8(b, wb, hb, cb);
    } catch (const std::exception& e) {
        Logger::log(LogLevel::Error, "Raw_equal: Error decoding " + b.string() + ": " + e.what(), get_name());
        return false;
    }

    // check for load failure
    if (pcmA.empty() || pcmB.empty()) {
        // error already logged by decode_tga_rgba8
        return false;
    }

    // compare dimensions
    if (wa != wb || ha != hb || ca != cb) {
        Logger::log(LogLevel::Debug, "Raw_equal: tga dimension mismatch", get_name());
        return false;
    }

    // compare raw pixel data
    if (pcmA != pcmB) {
        Logger::log(LogLevel::Debug, "Raw_equal: tga pixel data mismatch", get_name());
        return false;
    }

    return true;
}
} // namespace chisel