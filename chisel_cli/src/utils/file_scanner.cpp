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
    
    auto iequals = [](const std::string& a, const std::string& b) {
        return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                          [](char a, char b) { return tolower(a) == tolower(b); });
    };
    
    return iequals(name, ".ds_store") || iequals(name, "desktop.ini");
}

namespace {
bool is_filtered(const fs::path& path, const std::vector<std::regex>& exclude_regexes, const std::vector<std::regex>& include_regexes) {
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


std::vector<fs::path>
collect_input_files(const std::vector<fs::path>& inputs,
                    const Settings& settings,
                    bool& is_pipe) {
    std::vector<fs::path> result;
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
            fs::path tmp = fs::temp_directory_path() / "stdin_chisel.bin";
            std::ofstream out(tmp, std::ios::binary);
            out << std::cin.rdbuf();
            out.close();
            result.push_back(tmp);
            is_pipe = true;
            continue;
        }
        if (!fs::exists(in)) {
            chisel::Logger::log(chisel::LogLevel::Error, "Input not found: " + in.string(), "scanner");
            continue;
        }
        if (fs::is_directory(in)) {
            if (recursive) {
                for (const auto& e : fs::recursive_directory_iterator(in, fs::directory_options::skip_permission_denied)) {
                    if (fs::is_regular_file(e.path()) && !is_junk(e.path()) && !is_filtered(e.path(), exclude_regexes, include_regexes))
                        result.push_back(e.path());
                }
            } else {
                for (const auto& e : fs::directory_iterator(in, fs::directory_options::skip_permission_denied)) {
                    if (fs::is_regular_file(e.path()) && !is_junk(e.path()) && !is_filtered(e.path(), exclude_regexes, include_regexes))
                        result.push_back(e.path());
                }
            }
        } else if (fs::is_regular_file(in) && !is_junk(in) && !is_filtered(in, exclude_regexes, include_regexes)) {
            result.push_back(in);
        }
    }

    chisel::Logger::log(chisel::LogLevel::Info,
                "Scanner collected " + std::to_string(result.size()) + " files",
                "scanner");
    return result;
}