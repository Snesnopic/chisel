//
// Created by Giuseppe Francione on 19/10/25.
//

#include "mkv_processor.hpp"
#include <vector>
#include <string>
#include <stdexcept>
#include <system_error>
#include <filesystem>

// forward declaration of mkclean API
extern "C" int mkclean_optimize(int argc, char* argv[]);

namespace chisel {

void MkvProcessor::recompress(const std::filesystem::path& input,
                              const std::filesystem::path& output,
                              bool preserve_metadata) {
    Logger::log(LogLevel::Info, "Starting Matroska optimization: " + input.string(), "mkv_processor");

    namespace fs = std::filesystem;
    std::error_code ec;

    const fs::path temp_input = fs::temp_directory_path() / (input.filename().string() + ".tmp.mkv");
    fs::copy_file(input, temp_input, fs::copy_options::overwrite_existing, ec);

    if (ec) {
        Logger::log(LogLevel::Error, "Failed to create temporary copy: " + ec.message(), "mkv_processor");
        throw std::runtime_error("MkvProcessor: temp copy failed");
    }

    std::vector<std::string> args;
    args.emplace_back("mkclean");

    if (preserve_metadata) {
        args.emplace_back("--optimize");
        args.emplace_back("--keep-cues");
    } else {
        args.emplace_back("--optimize");
        args.emplace_back("--unsafe");
    }
    args.emplace_back("--quiet");
    args.push_back(temp_input.string());
    args.push_back(output.string());

    std::vector<char*> argv;
    argv.reserve(args.size());
    for (auto& s : args) {
        argv.push_back(s.data());
    }

    int return_code = -1;
    try {
        return_code = mkclean_optimize(static_cast<int>(argv.size()), argv.data());
    } catch (const std::exception& e) {
        Logger::log(LogLevel::Error, "mkclean_optimize exception: " + std::string(e.what()), "mkv_processor");
        fs::remove(temp_input, ec);
        throw;
    } catch (...) {
        Logger::log(LogLevel::Error, "mkclean_optimize unknown exception", "mkv_processor");
        fs::remove(temp_input, ec);
        throw;
    }

    fs::remove(temp_input, ec);

    if (return_code != 0) {
        Logger::log(LogLevel::Error, "mkclean failed with exit code " + std::to_string(return_code), "mkv_processor");
        throw std::runtime_error("MkvProcessor: mkclean failed");
    }

    Logger::log(LogLevel::Info, "Matroska optimization completed: " + output.string(), "mkv_processor");
}

std::optional<ExtractedContent> MkvProcessor::prepare_extraction(const std::filesystem::path&) {
    // TODO: implement track extraction in the future
    return std::nullopt;
}

void MkvProcessor::finalize_extraction(const ExtractedContent&,
                                       ContainerFormat) {
    // TODO: implement container rebuild after track modifications
}

std::string MkvProcessor::get_raw_checksum(const std::filesystem::path&) const {
    // TODO: implement checksum of raw streams
    return "";
}

} // namespace chisel