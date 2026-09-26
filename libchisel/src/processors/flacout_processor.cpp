//
// Created by Giuseppe Francione on 10/08/26.
//

#include "../../include/flacout_processor.hpp"
#include "../../include/logger.hpp"
#include "../../include/audio_metadata_util.hpp"
#include "../../include/random_utils.hpp"
#include <flacoutcpp.hpp>
#include <FLAC/all.h>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <vector>

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

    // flacoutcpp copies the metadata of a file that starts with the FLAC header: ID3v2 tags in front go back after
    const std::vector<uint8_t> id3 = AudioMetadataUtil::foreignId3v2Tags(input);
    std::filesystem::path source = input;
    if (!id3.empty()) {
        source = output;
        source += ".flac";
        AudioMetadataUtil::writeWithHead(input, source, {}, id3.size());
    }
    const bool optimised = flacoutcpp::optimise(source.string(), output.string(), cfg);
    if (source != input) {
        std::error_code ec;
        std::filesystem::remove(source, ec);
    }
    if (!optimised) {
        Logger::log(LogLevel::Error, "flacoutcpp optimise failed", get_name());
        throw std::runtime_error("flacoutcpp: optimise failed");
    }
    if (!id3.empty() && options.preserve_metadata) {
        AudioMetadataUtil::writeWithHead(output, output, id3, 0);
    }

    Logger::log(LogLevel::Debug, "Exiting recompress for " + output.string(), get_name());
}

std::optional<ExtractedContent> FlacoutProcessor::prepare_extraction(const std::filesystem::path& input_path) {
    return AudioMetadataUtil::prepareCoverExtraction(input_path, "flacout-processor", get_name());
}

std::filesystem::path FlacoutProcessor::finalize_extraction(const ExtractedContent &content, const ProcessingOptions &options) {
    return AudioMetadataUtil::finalizeCoverExtraction(content, get_name());
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
    // libFLAC doesn't find the stream behind ID3v2 tags followed by zero padding: decode it without them
    const auto decode = [](const std::filesystem::path& file, unsigned& rate, unsigned& channels, unsigned& bps) {
        const std::vector<uint8_t> id3 = AudioMetadataUtil::foreignId3v2Tags(file);
        if (id3.empty()) return decode_flac_pcm(file, rate, channels, bps);
        const std::filesystem::path stripped =
            std::filesystem::temp_directory_path() / ("flacout_raw_" + RandomUtils::random_suffix() + ".flac");
        AudioMetadataUtil::writeWithHead(file, stripped, {}, id3.size());
        std::vector<int32_t> pcm;
        try {
            pcm = decode_flac_pcm(stripped, rate, channels, bps);
        } catch (...) {
            std::error_code ec;
            std::filesystem::remove(stripped, ec);
            throw;
        }
        std::error_code ec;
        std::filesystem::remove(stripped, ec);
        return pcm;
    };
    unsigned ra, ca, bpsa;
    unsigned rb, cb, bpsb;
    const auto pcmA = decode(a, ra, ca, bpsa);
    const auto pcmB = decode(b, rb, cb, bpsb);

    if (ra != rb || ca != cb || bpsa != bpsb) return false;
    return pcmA == pcmB;
}

} // namespace chisel
