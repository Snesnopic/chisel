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
#include <fstream>
#include "../../../third_party/vbrfix/include/vbrfix/vbrfix.hpp"
#include "packer.hpp"
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
    /*
    try {
        vbrfix::FixParams params;
        params.always_skip = false;

        const std::vector<uint8_t> fixed_data = vbrfix::fix_mp3(output, params);

        std::ofstream ofs(output, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) {
            throw std::runtime_error("Failed to open output file for writing VBR fix data.");
        }

        ofs.write(reinterpret_cast<const char*>(fixed_data.data()), static_cast<std::streamsize>(fixed_data.size()));
        ofs.close();

    } catch (const std::exception& e) {
        throw std::runtime_error("Exception during VBR fix processing: " + std::string(e.what()));
    }

    Logger::log(LogLevel::Debug, "Compression and vbr fix successful.", get_name());
    */
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

} // namespace chisel