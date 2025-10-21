//
// Created by Giuseppe Francione on 20/09/25.
//

#include "file_scanner.hpp"
#include "../../libchisel/include/logger.hpp"
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

static bool is_junk(const fs::path& p) {
    auto name = p.filename().string();
    std::transform(name.begin(), name.end(), name.begin(), ::tolower);
    return name == ".ds_store" || name == "desktop.ini";
}

std::vector<fs::path>
collect_input_files(const std::vector<fs::path>& inputs,
                    const bool recursive,
                    bool& is_pipe) {
    std::vector<fs::path> result;

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
                    if (fs::is_regular_file(e.path()) && !is_junk(e.path()))
                        result.push_back(e.path());
                }
            } else {
                for (auto& e : fs::directory_iterator(in)) {
                    if (fs::is_regular_file(e.path()) && !is_junk(e.path()))
                        result.push_back(e.path());
                }
            }
        } else if (fs::is_regular_file(in) && !is_junk(in)) {
            result.push_back(in);
        }
    }

    Logger::log(LogLevel::Info,
                "Scanner collected " + std::to_string(result.size()) + " files",
                "scanner");
    return result;
}