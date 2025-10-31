//
// Created by Giuseppe Francione on 20/09/25.
//

#include "file_scanner.hpp"
#include "../cli/cli_parser.hpp"
#include "../../libchisel/include/logger.hpp"
#include <fstream>
#include <iostream>
#include <algorithm>
#include <regex>

namespace fs = std::filesystem;

static bool is_junk(const fs::path& p) {
    auto name = p.filename().string();
    if (name.starts_with("._")) {
        return true;
    }
    std::transform(name.begin(), name.end(), name.begin(), ::tolower);
    return name == ".ds_store" || name == "desktop.ini";
}

namespace {
bool is_filtered(const fs::path& path, const Settings& settings) {
    const std::string path_str = path.string();

    if (!settings.exclude_patterns.empty()) {
        for (const auto& pattern : settings.exclude_patterns) {
            try {
                if (std::regex_search(path_str, std::regex(pattern))) {
                    return true;
                }
            } catch (const std::regex_error& e) {
                Logger::log(LogLevel::Warning, "Invalid exclude regex: " + pattern + " (" + e.what() + ")", "scanner");
            }
        }
    }

    if (!settings.include_patterns.empty()) {
        for (const auto& pattern : settings.include_patterns) {
            try {
                if (std::regex_search(path_str, std::regex(pattern))) {
                    return false;
                }
            } catch (const std::regex_error& e) {
                Logger::log(LogLevel::Warning, "Invalid include regex: " + pattern + " (" + e.what() + ")", "scanner");
            }
        }
        return true;
    }

    return false;
}
} // namespace


std::vector<fs::path>
collect_input_files(const std::vector<fs::path>& inputs,
                    const Settings& settings,
                    bool& is_pipe) {
    std::vector<fs::path> result;
    const bool recursive = settings.recursive;

    for (const auto& in : inputs) {
        if (in == "-") {
            fs::path tmp = fs::temp_directory_path() / "stdin_chisel.bin";
            std::ofstream out(tmp, std::ios::binary);
            out << std::cin.rdbuf();
            out.close();
            result.push_back(tmp);
            is_pipe = true;
            continue;
        }
        if (!fs::exists(in)) {
            Logger::log(LogLevel::Error, "Input not found: " + in.string(), "scanner");
            continue;
        }
        if (fs::is_directory(in)) {
            if (recursive) {
                for (auto& e : fs::recursive_directory_iterator(in)) {
                    if (fs::is_regular_file(e.path()) && !is_junk(e.path()) && !is_filtered(e.path(), settings))
                        result.push_back(e.path());
                }
            } else {
                for (auto& e : fs::directory_iterator(in)) {
                    if (fs::is_regular_file(e.path()) && !is_junk(e.path()) && !is_filtered(e.path(), settings))
                        result.push_back(e.path());
                }
            }
        } else if (fs::is_regular_file(in) && !is_junk(in) && !is_filtered(in, settings)) {
            result.push_back(in);
        }
    }

    Logger::log(LogLevel::Info,
                "Scanner collected " + std::to_string(result.size()) + " files",
                "scanner");
    return result;
}