//
// Created by Giuseppe Francione on 15/08/26.
//

#include "../../include/optigif_processor.hpp"
#include "../../include/logger.hpp"
#include "../../include/gif_animation_compare.hpp"
#include "file_utils.hpp"
#include <optigif/optigif.hpp>
#include <cstdio>
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

    const auto result = gif_animations_equal(bufA, bufB);
    if (!result.equal) {
        Logger::log(LogLevel::Debug, "Raw_equal: " + result.reason, get_name());
    }
    return result.equal;
}

std::string OptigifProcessor::get_raw_checksum(const std::filesystem::path&) const {
    return "";
}

} // namespace chisel
