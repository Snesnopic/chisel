//
// Created by Giuseppe Francione on 18/11/25.
//

#include "../../include/mp4_processor.hpp"
#include "../../include/logger.hpp"
#include "../../include/audio_metadata_util.hpp"
#include "../../include/file_utils.hpp"
#include "../../include/random_utils.hpp"
#include <stdexcept>
#include <filesystem>
#include "file_type.hpp"


namespace chisel {
namespace fs = std::filesystem;

void Mp4Processor::recompress(const fs::path& input,
                              const fs::path& output, const ProcessingOptions &options) {
    Logger::log(LogLevel::Debug, "Entering recompress for " + input.string(), get_name());
    Logger::log(LogLevel::Warning, "Recompress called on Mp4Processor placeholder.", get_name());
    std::error_code ec;
    fs::copy_file(input, output, fs::copy_options::overwrite_existing, ec);
    if (ec) throw std::runtime_error("Placeholder recompress failed to copy file.");
    Logger::log(LogLevel::Debug, "Exiting recompress for " + output.string(), get_name());
}

std::optional<ExtractedContent> Mp4Processor::prepare_extraction(const fs::path& input_path) {
    Logger::log(LogLevel::Debug, "Entering prepare_extraction for " + input_path.string(), get_name());

    ExtractedContent content;
    content.original_path = input_path;
    content.temp_dir = make_temp_dir_for(input_path, "mp4-processor");

    AudioExtractionState state = AudioMetadataUtil::extractCovers(input_path, content.temp_dir);

    if (state.extracted_covers.empty()) {
        Logger::log(LogLevel::Info, "No embedded cover art found", get_name());
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

std::filesystem::path Mp4Processor::finalize_extraction(const ExtractedContent &content, const ProcessingOptions &options) {
    Logger::log(LogLevel::Debug, "Entering finalize_extraction for " + content.original_path.string(), get_name());

    const AudioExtractionState* state_ptr = std::any_cast<AudioExtractionState>(&content.extras);
    if (state_ptr == nullptr) {
        Logger::log(LogLevel::Error, "Failed to retrieve extraction state", get_name());
        cleanup_temp_dir(content.temp_dir, get_name());
        return {};
    }

    const fs::path final_temp_path = fs::temp_directory_path() /
                                     (content.original_path.stem().string() + "_final" + RandomUtils::random_suffix() + content.original_path.extension().string());

    try {
        fs::copy_file(content.original_path, final_temp_path, fs::copy_options::overwrite_existing);
    } catch (const std::exception& e) {
        Logger::log(LogLevel::Error, "Failed to copy file: " + std::string(e.what()), get_name());
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

} // namespace chisel