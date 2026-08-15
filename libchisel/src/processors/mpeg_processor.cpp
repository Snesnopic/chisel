//
// Created by Giuseppe Francione on 18/11/25.
//

#include "../../include/mpeg_processor.hpp"
#include "../../include/logger.hpp"
#include "../../include/audio_metadata_util.hpp"
#include "../../include/file_utils.hpp"
#include "../../include/random_utils.hpp"
#include <stdexcept>
#include <filesystem>
#include "packer.hpp"
#include "decoder.hpp"
#include "file_type.hpp"


namespace chisel {
namespace fs = std::filesystem;

void MpegProcessor::recompress(const fs::path& input,
                               const fs::path& output, const ProcessingOptions &options) {
    Logger::log(LogLevel::Debug, "Entering recompress for " + input.string(), get_name());
    Logger::log(LogLevel::Info, "Starting compression via mp3packercpp: " + input.string(), get_name());

    if (fs::exists(output)) {
        fs::remove(output);
    }

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

} // namespace chisel