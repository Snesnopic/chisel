//
// Created by Giuseppe Francione on 20/09/25.
//

#ifndef MONOLITH_CLI_PARSER_HPP
#define MONOLITH_CLI_PARSER_HPP

#include <string>
#include <vector>
#include <filesystem>
#include <optional>


enum class ContainerFormat;

struct Settings {
    bool preserve_metadata = true;
    bool recursive = false;
    bool dry_run = false;
    unsigned num_threads = 1;
    std::string log_level = "INFO";
    std::vector<std::filesystem::path> inputs;
    std::optional<ContainerFormat> unencodable_target_format;
    std::filesystem::path output_csv;
    bool is_pipe = false;
    bool regenerate_magic = false;
};

bool parse_arguments(int argc, char** argv, Settings& settings);

#endif //MONOLITH_CLI_PARSER_HPP