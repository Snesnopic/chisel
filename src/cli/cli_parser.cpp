//
// Created by Giuseppe Francione on 20/09/25.
//

#include "cli_parser.hpp"
#include "../utils/logger.hpp"
#include  "../utils/archive_formats.hpp"
#include "../containers/archive_handler.hpp"
#include <unordered_map>
#include <functional>
#include <algorithm>
#include <iostream>
#include <thread>


bool parse_arguments(const int argc, char** argv, Settings& settings) {
    if (argc < 2) {
        std::cerr << "Usage:\n"
                  << "  " << argv[0] << " <file_or_directory>... [options]\n\n"
                  << "Options:\n"
                  << "  --dry-run                  Use monolith without replacing original files.\n"
                  << "  --no-meta                  Don't preserve files metadata.\n"
                  << "  --recursive                Recursively scan input folders.\n"
                  << "  --threads N                Threads to use for parallel encoding.\n"
                  << "  --log-level LEVEL          Log level: ERROR, WARNING, INFO, DEBUG, NONE.\n"
                  << "  -o, --output-csv FILE      CSV report export filename.\n"
                  << "                             If not specified, report is printed on stdout.\n"
                  << "  --mode MODE                Encoding mode: 'pipe' (default) or 'parallel'.\n"
                  << "                             Pipe mode feeds the output of an encoder into \n"
                  << "                             the next one. Parallel mode runs all encoders \n"
                  << "                             on the original file, picking the best.\n"
                  << "  --regenerate-magic         Re-install libmagic file-detection database.\n"
                  << "  --recompress-unencodable FORMAT\n"
                  << "                             allows to recompress archives that can be opened but not recompressed\n"
                  << "                             into a different format (zip, 7z, tar, gz, bz2, xz, wim).\n"
                  << "                             if not specified, such archives are left untouched.\n\n"
                  << "Examples:\n"
                  << "  " << argv[0] << " file.jpg dir/ --recursive --threads 4\n"
                  << "  " << argv[0] << " archive.zip\n"
                  << "  " << argv[0] << " archive.rar --recompress-unencodable 7z\n"
                  << "  " << argv[0] << " dir/ -o report.csv\n";
        return false;
    }

    settings.num_threads = std::max(1U, std::thread::hardware_concurrency() / 2);

    using FlagHandler = std::function<void(int&, char**)>;
    std::unordered_map<std::string, FlagHandler> flag_map;

    flag_map["--no-meta"] = [&](const int&, char**) { settings.preserve_metadata = false; };
    flag_map["--recursive"] = [&](const int&, char**) { settings.recursive = true; };
    flag_map["--no-log"] = [&](const int&, char**) { Logger::enable(false); };
    flag_map["--dry-run"] = [&](const int&, char**) { settings.dry_run = true; };

    flag_map["--log-level"] = [&](int& i, char** args) {
        std::string lvl = args[++i];
        std::ranges::transform(lvl, lvl.begin(), ::toupper);
        if (lvl == "DEBUG") Logger::set_level(LogLevel::DEBUG);
        else if (lvl == "INFO") Logger::set_level(LogLevel::INFO);
        else if (lvl == "WARNING" || lvl == "WARN") Logger::set_level(LogLevel::WARNING);
        else if (lvl == "ERROR") Logger::set_level(LogLevel::ERROR);
        else if (lvl == "NONE") Logger::set_level(LogLevel::NONE);
        else throw std::runtime_error("Unknown log level: " + lvl);
    };

    flag_map["--threads"] = [&](int& i, char** args) {
        settings.num_threads = std::max(1UL, std::stoul(args[++i]));
    };

    flag_map["--recompress-unencodable"] = [&](int &i, char **args) {
        if (i + 1 >= argc) {
            std::cerr << "--recompress-unencodable requires a format (zip, 7z, tar, gz, bz2, xz, wim)\n";
            exit(1);
        }
        const std::string fmt_str = args[++i];
        auto fmt = parse_container_format(fmt_str);
        if (!fmt.has_value()) {
            std::cerr << "Format not valid for --recompress-unencodable: " << fmt_str << "\n";
            exit(1);
        }
        settings.unencodable_target_format = fmt.value();
    };

    flag_map["-o"] = [&](int &i, char **args) {
        const std::string out = args[++i];
        if (out == "-") {
            throw std::runtime_error("'-' is not allowed as output CSV (only files are supported).");
        }
        settings.output_csv = out;
    };
    flag_map["--output-csv"] = flag_map["-o"];

    flag_map["--regenerate-magic"] = [&](const int&, char**) {
        settings.regenerate_magic = true;
    };
    flag_map["--mode"] = [&](int& i, char** args) {
        if (i + 1 >= argc) {
            throw std::runtime_error("--mode requires an argument (pipe|parallel)");
        }
        std::string mode = args[++i];
        std::ranges::transform(mode, mode.begin(), ::tolower);
        if (mode == "pipe") {
            settings.encode_mode = EncodeMode::PIPE;
        } else if (mode == "parallel") {
            settings.encode_mode = EncodeMode::PARALLEL;
        } else {
            throw std::runtime_error("Unknown mode: " + mode + " (expected pipe or parallel)");
        }
    };

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (auto it = flag_map.find(a); it != flag_map.end()) {
            it->second(i, argv);
        } else if (!a.empty() && a[0] == '-' && a.size() > 1) {
            throw std::runtime_error("Unknown option: " + a);
        } else {
            settings.inputs.emplace_back(a);
        }
    }

    return true;
}