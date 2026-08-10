//
// Created by Giuseppe Francione on 10/08/26.
//

#include "../../include/flacout_processor.hpp"
#include "../../include/logger.hpp"
#include <flacoutcpp.hpp>
#include <FLAC/all.h>
#include <sstream>
#include <iomanip>
#include <stdexcept>

namespace chisel {

// defined in flac_processor.cpp, reused here instead of duplicating the decode loop
std::vector<int32_t> decode_flac_pcm(const std::filesystem::path& file,
                                     unsigned& sample_rate,
                                     unsigned& channels,
                                     unsigned& bps);

void FlacoutProcessor::recompress(const std::filesystem::path &input,
                                  const std::filesystem::path &output, const ProcessingOptions &options) {
    Logger::log(LogLevel::Debug, "Entering recompress for " + input.string(), get_name());

    flacoutcpp::Config cfg;
    cfg.copy_metadata = options.preserve_metadata;
    cfg.verbose = false;

    if (!flacoutcpp::optimise(input.string(), output.string(), cfg)) {
        Logger::log(LogLevel::Error, "flacoutcpp optimise failed", get_name());
        throw std::runtime_error("flacoutcpp: optimise failed");
    }

    Logger::log(LogLevel::Debug, "Exiting recompress for " + output.string(), get_name());
}

std::string FlacoutProcessor::get_raw_checksum(const std::filesystem::path& file_path) const {
    // FLAC__StreamMetadata has no internal allocation: pass the address of a
    // plain struct, not a pointer-to-pointer (there is nothing to free after)
    FLAC__StreamMetadata metadata;

    if (!FLAC__metadata_get_streaminfo(file_path.string().c_str(), &metadata)) {
        throw std::runtime_error("Failed to read STREAMINFO from FLAC file: " + file_path.string());
    }

    std::ostringstream oss;
    for (int i = 0; i < 16; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(metadata.data.stream_info.md5sum[i]);
    }

    return oss.str();
}

bool FlacoutProcessor::raw_equal(const std::filesystem::path& a,
                                 const std::filesystem::path& b) const {
    unsigned ra, ca, bpsa;
    unsigned rb, cb, bpsb;
    const auto pcmA = decode_flac_pcm(a, ra, ca, bpsa);
    const auto pcmB = decode_flac_pcm(b, rb, cb, bpsb);

    if (ra != rb || ca != cb || bpsa != bpsb) return false;
    return pcmA == pcmB;
}

} // namespace chisel
