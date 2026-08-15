//
// Created by Giuseppe Francione on 15/08/26.
//

#include "../../include/optigif_processor.hpp"
#include "../../include/logger.hpp"
#include "file_utils.hpp"
#include "stb_image.h"
#include <optigif/optigif.hpp>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace chisel {

void OptigifProcessor::recompress(const std::filesystem::path& input,
                                  const std::filesystem::path& output, const ProcessingOptions &options) {
    Logger::log(LogLevel::Debug, "Entering recompress for " + input.string(), get_name());

    optigif::Options opts;
    opts.restructure = true;
    opts.search_restarts = true;
    opts.strip_metadata = !options.preserve_metadata;
    // chisel already parallelizes across files via its own ThreadPool; running
    // optigif's own search/restructure threads on top would oversubscribe.
    opts.search.threads = 1;
    // search_restarts is O(n^2/alignment) per frame; the library default (160)
    // took 30+ min of single-thread CPU on this project's bench corpus without
    // finishing. 640 was measured on the same corpus (single thread, 51 files,
    // --strip) at -11.48% in 161s -- beating chisel's old gifsicle->flexigif
    // pipeline on both size (-11.43%) and time (737s, and only on 29/51 files
    // -- flexiGIF never finishes on the rest) with a 4.5x time margin.
    opts.search.alignment = 640;

    const auto result = optigif::recompress_file(input, output, opts);
    if (!result.ok) {
        Logger::log(LogLevel::Error, "optigif recompress_file failed", get_name());
        throw std::runtime_error("optigif: recompress_file failed for " + input.string());
    }

    Logger::log(LogLevel::Debug, "Exiting recompress for " + output.string(), get_name());
}

static std::vector<unsigned char> read_file_to_buffer_optigif(const std::filesystem::path& path) {
    FILE* f = chisel::open_file(path, "rb");
    if (!f) return {};
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        fclose(f);
        return {};
    }
    std::vector<unsigned char> buf(size);
    const std::size_t read_count = fread(buf.data(), 1, size, f);
    fclose(f);

    if (read_count != static_cast<std::size_t>(size)) {
        return {};
    }
    return buf;
}

bool OptigifProcessor::raw_equal(const std::filesystem::path& a, const std::filesystem::path& b) const {
    const auto bufA = read_file_to_buffer_optigif(a);
    const auto bufB = read_file_to_buffer_optigif(b);

    if (bufA.empty() || bufB.empty()) {
        Logger::log(LogLevel::Warning, "Raw_equal: empty or unreadable file(s)", get_name());
        return false;
    }

    int wA, hA, framesA;
    int* delaysA = nullptr;
    unsigned char* dataA = stbi_load_gif_from_memory(
        bufA.data(), static_cast<int>(bufA.size()),
        &delaysA, &wA, &hA, &framesA, nullptr, 4
    );

    if (!dataA) {
        Logger::log(LogLevel::Warning, "Raw_equal: failed to decode gif a", get_name());
        return false;
    }

    int wB, hB, framesB;
    int* delaysB = nullptr;
    unsigned char* dataB = stbi_load_gif_from_memory(
        bufB.data(), static_cast<int>(bufB.size()),
        &delaysB, &wB, &hB, &framesB, nullptr, 4
    );

    if (!dataB) {
        Logger::log(LogLevel::Warning, "Raw_equal: failed to decode gif b", get_name());
        stbi_image_free(dataA);
        if (delaysA) stbi_image_free(delaysA);
        return false;
    }

    bool equal = true;

    if (wA != wB || hA != hB || framesA != framesB) {
        Logger::log(LogLevel::Debug, "Raw_equal: dimension/frame count mismatch", get_name());
        equal = false;
    } else {
        const std::size_t totalBytes = static_cast<std::size_t>(wA) * hA * 4 * framesA;
        if (std::memcmp(dataA, dataB, totalBytes) != 0) {
            Logger::log(LogLevel::Debug, "Raw_equal: pixel mismatch", get_name());
            equal = false;
        }
        // pixel data alone doesn't capture animation timing: compare per-frame
        // delays too, since a recompressor could reproduce identical frames
        // while still corrupting playback speed
        if (equal && (delaysA == nullptr) != (delaysB == nullptr)) {
            Logger::log(LogLevel::Debug, "Raw_equal: delay array presence mismatch", get_name());
            equal = false;
        } else if (equal && delaysA && delaysB &&
                   std::memcmp(delaysA, delaysB, static_cast<std::size_t>(framesA) * sizeof(int)) != 0) {
            Logger::log(LogLevel::Debug, "Raw_equal: delay mismatch", get_name());
            equal = false;
        }
    }

    stbi_image_free(dataA);
    if (delaysA) stbi_image_free(delaysA);
    stbi_image_free(dataB);
    if (delaysB) stbi_image_free(delaysB);

    return equal;
}

std::string OptigifProcessor::get_raw_checksum(const std::filesystem::path&) const {
    return "";
}

} // namespace chisel
