//
// Created by Giuseppe Francione on 19/10/25.
//

#include "../../include/mkv_processor.hpp"
#include "../../include/logger.hpp"
#include "../../include/audio_metadata_util.hpp"
#include "../../include/random_utils.hpp"
#include "../../include/file_utils.hpp"
#include <vector>
#include <string>
#include <stdexcept>
#include <system_error>
#include <filesystem>

#ifdef HAVE_MATROSKA
// forward declaration of mkclean API
extern "C" int mkclean_optimize(int argc, char* argv[]);
#endif

namespace chisel {

void MkvProcessor::recompress(const std::filesystem::path& input,
                              const std::filesystem::path& output, const ProcessingOptions &options) {
    Logger::log(LogLevel::Debug, "entering recompress for " + input.string(), get_name());
#ifdef HAVE_MATROSKA
    std::vector<std::string> args;
    args.emplace_back("mkclean");

    if (options.preserve_metadata) {
        args.emplace_back("--optimize");
        args.emplace_back("--keep-cues");
    } else {
        args.emplace_back("--optimize");
        args.emplace_back("--unsafe");
    }

    args.emplace_back("--quiet");
    
#ifdef _WIN32
    // On Windows, use wide strings for paths if the API supports it, 
    // or at least ensure we use the absolute path string.
    std::error_code ec;
    auto abs_input = std::filesystem::absolute(input, ec).string();
    auto abs_output = std::filesystem::absolute(output, ec).string();
    args.push_back(abs_input);
    args.push_back(abs_output);
#else
    args.push_back(input.string());
    args.push_back(output.string());
#endif

    std::vector<char*> argv;
    argv.reserve(args.size());
    for (auto& s : args) {
        argv.push_back(s.data());
    }

    int return_code = -1;
    try {
        // mkclean works fine reading from input and writing to output directly
        return_code = mkclean_optimize(static_cast<int>(argv.size()), argv.data());
    } catch (const std::exception& e) {
        Logger::log(LogLevel::Warning, "mkclean_optimize exception (likely corrupt): " + std::string(e.what()), get_name());
        std::filesystem::copy_file(input, output, std::filesystem::copy_options::overwrite_existing);
        return;
    }

    if (return_code != 0) {
        Logger::log(LogLevel::Warning, "mkclean failed (likely corrupt file) with exit code " + std::to_string(return_code), get_name());
        std::filesystem::copy_file(input, output, std::filesystem::copy_options::overwrite_existing);
        return;
    }
#endif
    Logger::log(LogLevel::Debug, "exiting recompress for " + output.string(), get_name());
}

std::optional<ExtractedContent> MkvProcessor::prepare_extraction(const std::filesystem::path& input_path) {
    Logger::log(LogLevel::Debug, "entering prepare_extraction for " + input_path.string(), get_name());

    ExtractedContent content;
    content.original_path = input_path;
    content.temp_dir = make_temp_dir_for(input_path, "mkv-processor");

    AudioExtractionState state = AudioMetadataUtil::extractCovers(input_path, content.temp_dir);

    if (state.extracted_covers.empty()) {
        Logger::log(LogLevel::Info, "no embedded cover art or image attachments found", get_name());
        cleanup_temp_dir(content.temp_dir, get_name());
        return std::nullopt;
    }

    for (const auto& cover_info : state.extracted_covers) {
        content.extracted_files.push_back(cover_info.temp_file_path);
    }

    content.extras = std::make_any<AudioExtractionState>(std::move(state));
    content.format = ContainerFormat::Mkv;

    Logger::log(LogLevel::Debug, "exiting prepare_extraction for " + input_path.string(), get_name());
    return content;
}

std::filesystem::path MkvProcessor::finalize_extraction(const ExtractedContent &content, const ProcessingOptions &options) {
    Logger::log(LogLevel::Debug, "entering finalize_extraction for " + content.original_path.string(), get_name());

    const auto* state_ptr = std::any_cast<AudioExtractionState>(&content.extras);
    if (state_ptr == nullptr) {
        Logger::log(LogLevel::Error, "failed to retrieve extraction state", get_name());
        cleanup_temp_dir(content.temp_dir, get_name());
        return {};
    }

    // create a temporary file for the reconstruction
    const std::filesystem::path final_temp_path = std::filesystem::temp_directory_path() /
                                     (content.original_path.stem().string() + "_rebuilt" + RandomUtils::random_suffix() + ".mkv");

    try {
        // we start from the original file, rebuildCovers will replace attachments
        std::filesystem::copy_file(content.original_path, final_temp_path, std::filesystem::copy_options::overwrite_existing);
    } catch (const std::exception& e) {
        Logger::log(LogLevel::Error, "failed to copy mkv file: " + std::string(e.what()), get_name());
        cleanup_temp_dir(content.temp_dir, get_name());
        return {};
    }

    if (!AudioMetadataUtil::rebuildCovers(final_temp_path, *state_ptr)) {
        Logger::log(LogLevel::Error, "rebuildCovers failed", get_name());
        cleanup_temp_dir(content.temp_dir, get_name());
        std::filesystem::remove(final_temp_path);
        return {};
    }

    cleanup_temp_dir(content.temp_dir, get_name());
    Logger::log(LogLevel::Debug, "exiting finalize_extraction for " + final_temp_path.string(), get_name());

    // recompress() (mkclean) already ran in phase 2, on content.original_path,
    // before this attachment reinsertion - not after; the executor does not
    // call recompress() again on the path returned here
    return final_temp_path;
}

std::string MkvProcessor::get_raw_checksum(const std::filesystem::path& file_path) const {
    return "";
}

} // namespace chisel