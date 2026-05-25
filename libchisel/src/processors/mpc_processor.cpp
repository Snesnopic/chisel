//
// Created by Giuseppe Francione on 24/03/26.
//

#include "../../include/mpc_processor.hpp"
#include "../../include/logger.hpp"
#include "../../include/audio_metadata_util.hpp"
#include "../../include/file_utils.hpp"
#include "../../include/random_utils.hpp"
#include <stdexcept>
#include <filesystem>
#include "file_type.hpp"


namespace chisel {
namespace fs = std::filesystem;

void MpcProcessor::recompress(const fs::path& input,
                              const fs::path& output, const ProcessingOptions &options) {
    Logger::log(LogLevel::Error, "Recompress called on MpcProcessor placeholder.", this->get_name());

    std::error_code ec;
    fs::copy_file(input, output, fs::copy_options::overwrite_existing, ec);

    if (ec) {
        throw std::runtime_error("Placeholder recompress failed to copy file.");
    }
}

std::optional<ExtractedContent> MpcProcessor::prepare_extraction(const fs::path& input_path) {
    Logger::log(LogLevel::Info, "MPC: Preparing cover art extraction for: " + input_path.string(), this->get_name());

    ExtractedContent content;
    content.original_path = input_path;
    content.temp_dir = make_temp_dir_for(input_path, "mpc-processor");

    AudioExtractionState state = AudioMetadataUtil::extractCovers(input_path, content.temp_dir);

    if (state.extracted_covers.empty()) {
        Logger::log(LogLevel::Debug, "MPC: No embedded cover art found.", this->get_name());
        cleanup_temp_dir(content.temp_dir, this->get_name());
        return std::nullopt;
    }

    for (const auto& cover_info : state.extracted_covers) {
        content.extracted_files.push_back(cover_info.temp_file_path);
    }

    content.extras = std::make_any<AudioExtractionState>(std::move(state));
    content.format = ContainerFormat::Unknown;
    return content;
}

std::filesystem::path MpcProcessor::finalize_extraction(const ExtractedContent &content, const ProcessingOptions &options) {
    Logger::log(LogLevel::Info, "MPC: Finalizing (re-inserting covers) for: " + content.original_path.string(), this->get_name());

    const AudioExtractionState* state_ptr = std::any_cast<AudioExtractionState>(&content.extras);
    if (!state_ptr) {
        Logger::log(LogLevel::Error, "MPC: Failed to retrieve extraction state.", this->get_name());
        cleanup_temp_dir(content.temp_dir, this->get_name());
        return {};
    }

    const fs::path final_temp_path = fs::temp_directory_path() /
                                     (content.original_path.stem().string() + "_final" + RandomUtils::random_suffix() + ".mpc");

    try {
        fs::copy_file(content.original_path, final_temp_path, fs::copy_options::overwrite_existing);
    } catch (const std::exception& e) {
        Logger::log(LogLevel::Error, "MPC: Failed to copy audio file: " + std::string(e.what()), this->get_name());
        cleanup_temp_dir(content.temp_dir, this->get_name());
        return {};
    }

    if (!AudioMetadataUtil::rebuildCovers(final_temp_path, *state_ptr)) {
        Logger::log(LogLevel::Error, "MPC: rebuildCovers failed", this->get_name());
        cleanup_temp_dir(content.temp_dir, this->get_name());
        fs::remove(final_temp_path);
        return {};
    }

    cleanup_temp_dir(content.temp_dir, this->get_name());
    return final_temp_path;
}

} // namespace chisel