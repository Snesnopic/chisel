//
// Created by Giuseppe Francione on 18/11/25.
//

#include "../../include/wav_processor.hpp"
#include "../../include/logger.hpp"
#include "../../include/audio_metadata_util.hpp"
#include "../../include/file_utils.hpp"
#include "../../include/random_utils.hpp"
#include <stdexcept>
#include <filesystem>
#include "file_type.hpp"

namespace chisel {
namespace fs = std::filesystem;

static const char* processor_tag() {
    return "WavProcessor";
}

void WavProcessor::recompress(const fs::path& input,
                              const fs::path& output,
                              bool /*preserve_metadata*/) {
    Logger::log(LogLevel::Error, "Recompress called on WavProcessor placeholder.", processor_tag());

    std::error_code ec;
    fs::copy_file(input, output, fs::copy_options::overwrite_existing, ec);

    if (ec) {
        throw std::runtime_error("Placeholder recompress failed to copy file.");
    }
}

std::optional<ExtractedContent> WavProcessor::prepare_extraction(const fs::path& input_path) {
    Logger::log(LogLevel::Info, "WAV: Preparing cover art extraction for: " + input_path.string(), processor_tag());

    ExtractedContent content;
    content.original_path = input_path;
    content.temp_dir = make_temp_dir_for(input_path, "wav-processor");

    AudioExtractionState state = AudioMetadataUtil::extractCovers(input_path, content.temp_dir);

    if (state.extracted_covers.empty()) {
        Logger::log(LogLevel::Debug, "WAV: No embedded cover art found.", processor_tag());
        cleanup_temp_dir(content.temp_dir, processor_tag());
        return std::nullopt;
    }

    for (const auto& cover_info : state.extracted_covers) {
        content.extracted_files.push_back(cover_info.temp_file_path);
    }

    content.extras = std::make_any<AudioExtractionState>(std::move(state));
    content.format = ContainerFormat::Unknown;
    return content;
}

std::filesystem::path WavProcessor::finalize_extraction(const ExtractedContent &content) {
    Logger::log(LogLevel::Info, "WAV: Finalizing (re-inserting covers) for: " + content.original_path.string(), processor_tag());

    const AudioExtractionState* state_ptr = std::any_cast<AudioExtractionState>(&content.extras);
    if (!state_ptr) {
        Logger::log(LogLevel::Error, "WAV: Failed to retrieve extraction state.", processor_tag());
        cleanup_temp_dir(content.temp_dir, processor_tag());
        return {};
    }

    const fs::path final_temp_path = fs::temp_directory_path() /
                                     (content.original_path.stem().string() + "_final" + RandomUtils::random_suffix() + ".wav");

    try {
        fs::copy_file(content.original_path, final_temp_path, fs::copy_options::overwrite_existing);
    } catch (const std::exception& e) {
        Logger::log(LogLevel::Error, "WAV: Failed to copy audio file: " + std::string(e.what()), processor_tag());
        cleanup_temp_dir(content.temp_dir, processor_tag());
        return {};
    }

    if (!AudioMetadataUtil::rebuildCovers(final_temp_path, *state_ptr)) {
        Logger::log(LogLevel::Error, "WAV: rebuildCovers failed", processor_tag());
        cleanup_temp_dir(content.temp_dir, processor_tag());
        fs::remove(final_temp_path);
        return {};
    }

    cleanup_temp_dir(content.temp_dir, processor_tag());
    return final_temp_path;
}

} // namespace chisel