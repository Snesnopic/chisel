//
// Created by Giuseppe Francione on 20/09/25.
//

#ifndef CHISEL_CLI_PARSER_HPP
#define CHISEL_CLI_PARSER_HPP

#include <string>
#include <vector>
#include <filesystem>
#include <optional>


enum class ContainerFormat;

enum class EncodeMode {
    PIPE,      // one encoder output is the next one's output
    PARALLEL   // all encoders on the original file
};

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
};

bool parse_arguments(int argc, char** argv, Settings& settings);

#endif //CHISEL_CLI_PARSER_HPP