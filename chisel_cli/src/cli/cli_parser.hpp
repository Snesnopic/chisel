//
// Created by Giuseppe Francione on 20/09/25.
//

#ifndef CHISEL_CLI_PARSER_HPP
#define CHISEL_CLI_PARSER_HPP

#include <string>
#include <vector>
#include <filesystem>
#include <optional>
#include "../../libchisel/include/processor_executor.hpp"

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
    bool verify_checksums = false;
    EncodeMode encode_mode = EncodeMode::PIPE;

    bool quiet = false;
    std::filesystem::path output_dir;
    std::vector<std::string> include_patterns;
    std::vector<std::string> exclude_patterns;
};

bool parse_arguments(int argc, char** argv, Settings& settings);

#endif //CHISEL_CLI_PARSER_HPP