//
// Created by Giuseppe Francione on 20/09/25.
//

#ifndef CHISEL_REPORT_GENERATOR_HPP
#define CHISEL_REPORT_GENERATOR_HPP

#include <vector>
#include <string>
#include <filesystem>
#include <optional>
#include "processor_executor.hpp"

struct Result {
    std::filesystem::path path;
    std::optional<std::filesystem::path> container_origin;
    std::string mime;           // detected mime
    uintmax_t size_before{};    // original size in bytes
    uintmax_t size_after{};     // recompressed size in bytes
    bool success{};             // operation succeeded
    bool replaced{};            // file was replaced or skipped
    double seconds{};           // processing time
    std::vector<std::pair<std::string,double>> codecs_used; // codec name + % reduction
    std::string error_msg;      // if !success, reason of failure
    std::string skip_reason;    // set when the run left it as it is
    std::string inside;         // the containers around it, outermost first
};

struct ContainerResult {
    std::filesystem::path filename;
    std::optional<std::filesystem::path> parent;
    std::string format;
    uintmax_t size_before{};
    uintmax_t size_after{};
    bool success{};
    std::string error_msg;
    std::string skip_reason;
    std::string inside;
};

void print_console_report(const std::vector<Result>& results,
                          const std::vector<ContainerResult>& container_results,
                          unsigned num_threads,
                          double total_seconds,
                          chisel::EncodeMode mode
                          );

void export_csv_report(const std::vector<Result>& results,
                       const std::vector<ContainerResult>& container_results,
                       const std::filesystem::path& output_path,
                       double total_seconds,
                       chisel::EncodeMode mode);

unsigned get_terminal_width();

#endif //CHISEL_REPORT_GENERATOR_HPP