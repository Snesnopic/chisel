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

static bool is_junk(const std::filesystem::path& path) {
    const auto name = path.filename().string();
    if (name.starts_with("._")) {
        return true;
    }
    
    auto iequals = [](const std::string& a, const std::string& b) {
        return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                          [](const char a, const char b) { return tolower(a) == tolower(b); });
    };
    
    return iequals(name, ".ds_store") || iequals(name, "desktop.ini");
}

namespace {
bool is_filtered(const std::filesystem::path& path, const std::vector<std::regex>& exclude_regexes, const std::vector<std::regex>& include_regexes) {
    const std::string path_str = path.string();

    if (!exclude_regexes.empty()) {
        for (const auto& pattern : exclude_regexes) {
            if (std::regex_search(path_str, pattern)) {
                return true;
            }
        }
    }

    if (!include_regexes.empty()) {
        for (const auto& pattern : include_regexes) {
            if (std::regex_search(path_str, pattern)) {
                return false;
            }
        }
        return true;
    }

    return false;
}
} // namespace


std::vector<std::filesystem::path>
collect_input_files(const std::vector<std::filesystem::path>& inputs,
                    const Settings& settings,
                    bool& is_pipe) {
    std::vector<std::filesystem::path> result;
    const bool recursive = settings.recursive;

    std::vector<std::regex> exclude_regexes;
    for (const auto& pattern : settings.exclude_patterns) {
        try {
            exclude_regexes.emplace_back(pattern);
        } catch (const std::regex_error& e) {
            chisel::Logger::log(chisel::LogLevel::Warning, "Invalid exclude regex: " + pattern + " (" + e.what() + ")", "scanner");
        }
    }

    std::vector<std::regex> include_regexes;
    for (const auto& pattern : settings.include_patterns) {
        try {
            include_regexes.emplace_back(pattern);
        } catch (const std::regex_error& e) {
            chisel::Logger::log(chisel::LogLevel::Warning, "Invalid include regex: " + pattern + " (" + e.what() + ")", "scanner");
        }
    }

    for (const auto& in : inputs) {
        if (in == "-") {
            std::filesystem::path tmp = std::filesystem::temp_directory_path() / "stdin_chisel.bin";
            std::ofstream out(tmp, std::ios::binary);
            out << std::cin.rdbuf();
            out.close();
            result.push_back(tmp);
            is_pipe = true;
            continue;
        }
        if (!std::filesystem::exists(in)) {
            chisel::Logger::log(chisel::LogLevel::Error, "Input not found: " + in.string(), "scanner");
            continue;
        }
        if (std::filesystem::is_directory(in)) {
            if (recursive) {
                for (const auto& e : std::filesystem::recursive_directory_iterator(in, std::filesystem::directory_options::skip_permission_denied)) {
                    if (std::filesystem::is_regular_file(e.path()) && !is_junk(e.path()) && !is_filtered(e.path(), exclude_regexes, include_regexes))
                        result.push_back(e.path());
                }
            } else {
                for (const auto& e : std::filesystem::directory_iterator(in, std::filesystem::directory_options::skip_permission_denied)) {
                    if (std::filesystem::is_regular_file(e.path()) && !is_junk(e.path()) && !is_filtered(e.path(), exclude_regexes, include_regexes))
                        result.push_back(e.path());
                }
            }
        } else if (std::filesystem::is_regular_file(in) && !is_junk(in) && !is_filtered(in, exclude_regexes, include_regexes)) {
            result.push_back(in);
        }
    }

    chisel::Logger::log(chisel::LogLevel::Info,
                "Scanner collected " + std::to_string(result.size()) + " files",
                "scanner");
    return result;
}