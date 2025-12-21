//
// Created by Giuseppe Francione on 20/09/25.
//

#include "cli_parser.hpp"
#include "../../libchisel/include/file_type.hpp"
#include "CLI11.hpp"
#include <thread>
#include <algorithm>
#include <map>

namespace {
// helper for validating container format string
struct ContainerFormatValidator : CLI::Validator {
    ContainerFormatValidator() {
        name_ = "ContainerFormat";
        func_ = [](const std::string& str) {
            auto fmt = parse_container_format(str);
            if (!fmt.has_value()) {
                return std::string("Invalid format: '") + str +
                       "'. Must be one of: zip, 7z, tar, gz, bz2, xz, wim.";
            }
            return std::string(); // ok
        };
    }
};
} // namespace

void setup_cli_parser(CLI::App& app, Settings& settings) {
    // setup standard help and version flags
    app.set_help_flag("-h,--help", "Show this help message and exit.");
    app.set_version_flag("--version", "0.1");

    // --- Flags (booleans) ---
    app.add_flag("--no-meta", settings.no_meta,
                    "Don't preserve files metadata.");

    app.add_flag("-r,--recursive", settings.recursive,
                 "Recursively scan input folders.");

    app.add_flag("--dry-run", settings.dry_run,
                 "Use chisel without replacing original files.");

    app.add_flag("-q,--quiet", settings.quiet,
                 "Suppress non-error console output (progress bar, results).");

    app.add_flag("--regenerate-magic", settings.regenerate_magic,
                 "Re-install libmagic file-detection database.");

    app.add_flag("--verify-checksums", settings.verify_checksums,
                 "Verify raw checksums before replacing files.");

    app.add_option("-o,--output", settings.output_path,
                   "Write optimized files to PATH instead of modifying in-place.\n"
                   "(If input is stdin, PATH is a file. Otherwise, PATH is a directory).");

    app.add_option("--report", settings.report_path,
                   "CSV report export filename.")
                   ->take_last(); // if used multiple times, take the last one

    // calculate default thread count
    settings.num_threads = std::max(1U, std::thread::hardware_concurrency() / 2);
    app.add_option("--threads", settings.num_threads,
                   "Threads to use for parallel encoding.")
                   ->default_val(settings.num_threads)
                   ->check(CLI::PositiveNumber);

    app.add_option("--log-level", settings.log_level,
                   "Log level: ERROR, WARNING, INFO, DEBUG, NONE.")
                   ->default_val("ERROR")
                   ->check(CLI::IsMember({"ERROR", "WARNING", "INFO", "DEBUG", "NONE"}, CLI::ignore_case));

    app.add_option("--log-file", settings.log_file,
                   "Write logs to a specific file (default: no file logging).");

    // encoding mode option with a map transformer
    app.add_option("--mode", settings.encode_mode, "Encoding mode: 'pipe' (default) or 'parallel'.")
        ->default_val(EncodeMode::PIPE)
        ->transform(CLI::CheckedTransformer(
            std::map<std::string, EncodeMode>{
                {"pipe", EncodeMode::PIPE},
                {"parallel", EncodeMode::PARALLEL}
            }, CLI::ignore_case));

    app.add_option("--include", settings.include_patterns,
                   "Process only files matching regex PATTERN. (Can be used multiple times).");

    app.add_option("--exclude", settings.exclude_patterns,
                   "Do not process files matching regex PATTERN. (Can be used multiple times).");

    // --- Positional Arguments ---
    app.add_option("inputs", settings.inputs, "One or more files or directories (use '-' for stdin)")
        ->required()
        ->check([](const std::string& str) {
            // allow stdin or an existing path
            if (str == "-") return std::string();
            if (!std::filesystem::exists(str)) return "Input path '" + str + "' not found.";
            return std::string(); // ok
        });

    // --- Cross-validation logic ---
    app.callback([&settings]() {
        // check for stdin '-'
        for (const auto& path : settings.inputs) {
            if (path == "-") {
                settings.is_pipe = true;
                break;
            }
        }

        if (settings.is_pipe && settings.inputs.size() > 1) {
             throw CLI::ValidationError("Cannot use stdin ('-') with other input files.");
        }

        if (settings.is_pipe && settings.output_path.empty()) {
            throw CLI::ValidationError("Option '-o, --output' is required when using stdin ('-').");
        }

        if (settings.is_pipe && !settings.output_path.empty() && std::filesystem::is_directory(settings.output_path)) {
            throw CLI::ValidationError("Output path ('-o') must be a file, not a directory, when using stdin ('-').");
        }

        if (settings.dry_run && !settings.output_path.empty()) {
            throw CLI::ValidationError("--dry-run and -o, --output cannot be used together.");
        }
    });
}