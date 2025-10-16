//
// Created by Giuseppe Francione on 16/10/25.
//

#include "mkv_encoder.hpp"
#include "../utils/logger.hpp"
#include <vector>
#include <string>
#include <stdexcept>
#include <system_error>


MkvEncoder::MkvEncoder(const bool preserve_metadata) {
    preserve_metadata_ = preserve_metadata;
}

bool MkvEncoder::recompress(const std::filesystem::path &input,
                             const std::filesystem::path &output) {
    Logger::log(LogLevel::Info, "Starting Matroska optimization: " + input.string(), name());

    namespace fs = std::filesystem;
    std::error_code ec;

    const fs::path temp_input = fs::temp_directory_path() / (input.filename().string() + ".tmp.mkv");
    fs::copy_file(input, temp_input, fs::copy_options::overwrite_existing, ec);

    if (ec) {
        Logger::log(LogLevel::Error, "Failed to create temporary copy: " + ec.message(), name());
        return false;
    }

    std::vector<std::string> args;
    args.emplace_back("mkclean");

    if (preserve_metadata_) {
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
        Logger::log(LogLevel::Error, "mkclean_optimize threw an exception: " + std::string(e.what()), name());
        fs::remove(temp_input, ec);
        return false;
    } catch (...) {
        Logger::log(LogLevel::Error, "mkclean_optimize threw an unknown exception.", name());
        fs::remove(temp_input, ec);
        return false;
    }

    fs::remove(temp_input, ec);

    if (return_code != 0) {
        Logger::log(LogLevel::Error, "mkclean failed with exit code " + std::to_string(return_code), name());
        return false;
    }

    Logger::log(LogLevel::Info, "Matroska optimization completed: " + output.string(), name());
    return true;
}