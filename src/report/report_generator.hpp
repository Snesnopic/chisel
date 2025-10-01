//
// Created by Giuseppe Francione on 20/09/25.
//

#ifndef MONOLITH_REPORT_GENERATOR_HPP
#define MONOLITH_REPORT_GENERATOR_HPP

#include <vector>
#include <string>
#include <filesystem>

struct Result {
    std::string filename;       // full path or relative
    std::string mime;           // detected mime
    uintmax_t size_before{};    // original size in bytes
    uintmax_t size_after{};     // recompressed size in bytes
    bool success{};             // operation succeeded
    bool replaced{};            // file was replaced or skipped
    double seconds{};           // processing time
    std::vector<std::pair<std::string,double>> codecs_used; // codec name + % reduction
    std::string error_msg;      // if !success, reason of failure
};

void print_console_report(const std::vector<Result>& results,
                          unsigned num_threads,
                          double total_seconds);

void export_csv_report(const std::vector<Result>& results,
                       const std::filesystem::path& output_path);

unsigned get_terminal_width();

#endif //MONOLITH_REPORT_GENERATOR_HPP