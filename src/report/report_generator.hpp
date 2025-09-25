//
// Created by Giuseppe Francione on 20/09/25.
//

#ifndef MONOLITH_REPORT_GENERATOR_HPP
#define MONOLITH_REPORT_GENERATOR_HPP

#include <vector>
#include <string>
#include <filesystem>

struct Result {
    std::string filename;
    std::string mime;
    uintmax_t size_before{};
    uintmax_t size_after{};
    bool success{};
    bool replaced{};
    double seconds{};
};

void print_console_report(const std::vector<Result>& results,
                          unsigned num_threads,
                          double total_seconds);

void export_csv_report(const std::vector<Result>& results,
                       const std::filesystem::path& output_path);

#endif //MONOLITH_REPORT_GENERATOR_HPP