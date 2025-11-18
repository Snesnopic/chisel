//
// Created by Giuseppe Francione on 18/11/25.
//

#include "../../include/ogg_processor.hpp"
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
    return "OggProcessor";
}

void OggProcessor::recompress(const fs::path& input,
                              const fs::path& output,
                              bool /*preserve_metadata*/) {
    Logger::log(LogLevel::Error, "Recompress called on OggProcessor placeholder.", processor_tag());
    std::error_code ec;
    fs::copy_file(input, output, fs::copy_options::overwrite_existing, ec);
    if (ec) throw std::runtime_error("Placeholder recompress failed to copy file.");
}

std::optional<ExtractedContent> OggProcessor::prepare_extraction(const fs::path& input_path) {
    Logger::log(LogLevel::Info, "OGG: Preparing cover art extraction for: " + input_path.string(), processor_tag());

    ExtractedContent content;
    content.original_path = input_path;
    content.temp_dir = make_temp_dir_for(input_path, "ogg-processor");

    AudioExtractionState state = AudioMetadataUtil::extractCovers(input_path, content.temp_dir);

    if (state.extracted_covers.empty()) {
        Logger::log(LogLevel::Debug, "OGG: No embedded cover art found.", processor_tag());
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

std::filesystem::path OggProcessor::finalize_extraction(const ExtractedContent &content,
                                                        ContainerFormat /*target_format*/) {
    Logger::log(LogLevel::Info, "OGG: Finalizing (re-inserting covers) for: " + content.original_path.string(), processor_tag());

    const AudioExtractionState* state_ptr = std::any_cast<AudioExtractionState>(&content.extras);
    if (!state_ptr) {
        Logger::log(LogLevel::Error, "OGG: Failed to retrieve extraction state.", processor_tag());
        cleanup_temp_dir(content.temp_dir, processor_tag());
        return {};
    }

    const fs::path final_temp_path = fs::temp_directory_path() /
                                     (content.original_path.stem().string() + "_final" + RandomUtils::random_suffix() + content.original_path.extension().string());

    try {
        fs::copy_file(content.original_path, final_temp_path, fs::copy_options::overwrite_existing);
    } catch (const std::exception& e) {
        Logger::log(LogLevel::Error, "OGG: Failed to copy audio file: " + std::string(e.what()), processor_tag());
        cleanup_temp_dir(content.temp_dir, processor_tag());
        return {};
    }

    if (!AudioMetadataUtil::rebuildCovers(final_temp_path, *state_ptr)) {
        Logger::log(LogLevel::Error, "OGG: rebuildCovers failed", processor_tag());
        cleanup_temp_dir(content.temp_dir, processor_tag());
        fs::remove(final_temp_path);
        return {};
    }

    cleanup_temp_dir(content.temp_dir, processor_tag());
    return final_temp_path;
}

} // namespace chisel