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

// forward declaration
namespace CLI { class App; }
enum class ContainerFormat;

struct Settings {
    bool no_meta = false;
    bool recursive = false;
    bool dry_run = false;
    bool quiet = false;
    bool verify_checksums = false;
    bool regenerate_magic = false;

    unsigned num_threads = 1;
    std::string log_level = "INFO";
    std::filesystem::path output_path;
    std::filesystem::path report_path;
    std::optional<ContainerFormat> unencodable_target_format;
    EncodeMode encode_mode = EncodeMode::PIPE;
    std::vector<std::string> include_patterns;
    std::vector<std::string> exclude_patterns;

    std::vector<std::filesystem::path> inputs;

    bool is_pipe = false;
    [[nodiscard]] bool should_preserve_metadata() const { return !no_meta; }
};

/**
 * @brief Configures the CLI11 parser with all options, flags, and arguments.
 * @param app The CLI::App instance to configure.
 * @param settings The Settings struct to map the options to.
 */
void setup_cli_parser(CLI::App& app, Settings& settings);

#endif //CHISEL_CLI_PARSER_HPP