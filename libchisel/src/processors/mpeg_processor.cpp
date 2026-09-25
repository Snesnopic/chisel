//
// Created by Giuseppe Francione on 18/11/25.
//

#include "../../include/mpeg_processor.hpp"
#include "../../include/c2pa_manifest.hpp"
#include "../../include/logger.hpp"
#include "../../include/audio_metadata_util.hpp"
#include "../../include/file_utils.hpp"
#include "../../include/random_utils.hpp"
#include <stdexcept>
#include <filesystem>
#include <fstream>
#include <optional>
#include <vector>
#include "packer.hpp"
#include "decoder.hpp"
#include "file_type.hpp"


namespace chisel {
namespace fs = std::filesystem;

namespace {
namespace mpeg_layer {

// kbit/s by [mpeg-1, mpeg-2 or 2.5][layer i, ii, iii][index]
constexpr int kBitrates[2][3][15] = {
    {{0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448},
     {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384},
     {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320}},
    {{0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256},
     {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},
     {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160}}};
constexpr int kSampleRates[3] = {44100, 48000, 32000};

struct Frame {
    int layer = 0; ///< 1, 2 or 3
    std::size_t size = 0;
};

// an mpeg audio frame header; free-format bitrates aren't recognized
std::optional<Frame> parse_header(const unsigned char* h) {
    if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) return std::nullopt;
    const int version = (h[1] >> 3) & 3;
    const int layer = 4 - ((h[1] >> 1) & 3);
    const int bitrate = h[2] >> 4;
    const int rate = (h[2] >> 2) & 3;
    if (version == 1 || layer == 4 || bitrate == 0 || bitrate == 15 || rate == 3) return std::nullopt;
    const bool mpeg1 = version == 3;
    // mpeg-2 halves the sample rates, mpeg-2.5 quarters them
    const long hz = kSampleRates[rate] >> (mpeg1 ? 0 : version == 2 ? 1 : 2);
    const long bps = kBitrates[mpeg1 ? 0 : 1][layer - 1][bitrate] * 1000L;
    const long padding = (h[2] >> 1) & 1;
    long size;
    if (layer == 1) size = (12 * bps / hz + padding) * 4;
    else if (layer == 3 && !mpeg1) size = 72 * bps / hz + padding;
    else size = 144 * bps / hz + padding;
    return Frame{.layer = layer, .size = static_cast<std::size_t>(size)};
}

/**
 * @brief The layer of the audio after the ID3v2 tag, told by two frames in a row.
 * @return 1 to 3, or 0 when no such frames are found.
 */
int audio_layer(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    unsigned char tag[10]{};
    in.read(reinterpret_cast<char*>(tag), sizeof tag);
    std::streamoff start = 0;
    if (in.gcount() == sizeof tag && tag[0] == 'I' && tag[1] == 'D' && tag[2] == '3') {
        start = 10 + ((tag[6] & 0x7F) << 21 | (tag[7] & 0x7F) << 14 | (tag[8] & 0x7F) << 7 | (tag[9] & 0x7F));
        if (tag[5] & 0x10) start += 10;
    }
    in.clear();
    in.seekg(start);
    std::vector<unsigned char> head(1 << 16);
    in.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<std::size_t>(in.gcount()));
    for (std::size_t pos = 0; pos + 4 <= head.size(); ++pos) {
        const auto frame = parse_header(&head[pos]);
        if (!frame || pos + frame->size + 4 > head.size()) continue;
        const auto next = parse_header(&head[pos + frame->size]);
        if (next && next->layer == frame->layer) return frame->layer;
    }
    return 0;
}

} // namespace mpeg_layer
} // namespace

void MpegProcessor::recompress(const fs::path& input,
                               const fs::path& output, const ProcessingOptions &options) {
    Logger::log(LogLevel::Debug, "Entering recompress for " + input.string(), get_name());

    if (fs::exists(output)) {
        fs::remove(output);
    }

    // mp3packer rewrites layer iii frames only: mp1 and mp2 audio stays as it is, while its tags are still handled
    if (const int layer = mpeg_layer::audio_layer(input); layer == 1 || layer == 2) {
        Logger::log(LogLevel::Debug, std::string("MPEG audio layer ") + (layer == 1 ? "I" : "II") +
                    ", left as is: " + input.filename().string(), get_name());
        fs::copy_file(input, output);
        return;
    }

    Logger::log(LogLevel::Info, "Starting compression via mp3packercpp: " + input.string(), get_name());

    try {
        mp3packer::Packer packer;
        packer.recompress_huffman = true;
        packer.process(input.string(), output.string());
    } catch (const std::exception& e) {
        throw std::runtime_error("Exception during mp3packercpp execution: " + std::string(e.what()));
    }

    Logger::log(LogLevel::Debug, "Compression successful.", get_name());
    Logger::log(LogLevel::Debug, "Exiting recompress for " + output.string(), get_name());
}

std::optional<ExtractedContent> MpegProcessor::prepare_extraction(const fs::path& input_path) {
    Logger::log(LogLevel::Debug, "Entering prepare_extraction for " + input_path.string(), get_name());

    ExtractedContent content;
    content.original_path = input_path;
    content.temp_dir = make_temp_dir_for(input_path, "mp3-processor");

    AudioExtractionState state = AudioMetadataUtil::extractCovers(input_path, content.temp_dir);

    if (state.extracted_covers.empty()) {
        Logger::log(LogLevel::Info, "No embedded cover art found.", get_name());
        cleanup_temp_dir(content.temp_dir, get_name());
        return std::nullopt;
    }

    for (const auto& cover_info : state.extracted_covers) {
        content.extracted_files.push_back(cover_info.temp_file_path);
    }

    content.extras = std::make_any<AudioExtractionState>(std::move(state));
    content.format = ContainerFormat::Unknown;

    Logger::log(LogLevel::Debug, "Exiting prepare_extraction for " + input_path.string(), get_name());
    return content;
}

std::filesystem::path MpegProcessor::finalize_extraction(const ExtractedContent &content, const ProcessingOptions &options) {
    Logger::log(LogLevel::Debug, "Entering finalize_extraction for " + content.original_path.string(), get_name());

    const AudioExtractionState* state_ptr = std::any_cast<AudioExtractionState>(&content.extras);
    if (state_ptr == nullptr) {
        Logger::log(LogLevel::Error, "Failed to retrieve extraction state.", get_name());
        cleanup_temp_dir(content.temp_dir, get_name());
        return {};
    }

    fs::path final_temp_path = fs::temp_directory_path() /
                                     (content.original_path.stem().string() + "_final" + RandomUtils::random_suffix() + ".mp3");

    try {
        fs::copy_file(content.original_path, final_temp_path, fs::copy_options::overwrite_existing);
    } catch (const std::exception& e) {
        Logger::log(LogLevel::Error, "Failed to copy audio file: " + std::string(e.what()), get_name());
        cleanup_temp_dir(content.temp_dir, get_name());
        return {};
    }

    if (!AudioMetadataUtil::rebuildCovers(final_temp_path, *state_ptr)) {
        Logger::log(LogLevel::Error, "RebuildCovers failed", get_name());
        cleanup_temp_dir(content.temp_dir, get_name());
        fs::remove(final_temp_path);
        return {};
    }

    cleanup_temp_dir(content.temp_dir, get_name());
    Logger::log(LogLevel::Debug, "Exiting finalize_extraction for " + final_temp_path.string(), get_name());
    return final_temp_path;
}

bool MpegProcessor::raw_equal(const std::filesystem::path &a,
                              const std::filesystem::path &b) const {
    mp3packer::PcmAudio pcm_a, pcm_b;
    try {
        pcm_a = mp3packer::decode_pcm(a.string());
        pcm_b = mp3packer::decode_pcm(b.string());
    } catch (const std::exception& e) {
        Logger::log(LogLevel::Warning, std::string("raw_equal: failed to decode: ") + e.what(), get_name());
        return false;
    }

    if (pcm_a.channels != pcm_b.channels || pcm_a.sample_rate != pcm_b.sample_rate) return false;
    return pcm_a.samples == pcm_b.samples;
}

bool MpegProcessor::is_signed(const std::filesystem::path& file_path) const {
    return c2pa::id3_has_manifest(file_path);
}

} // namespace chisel