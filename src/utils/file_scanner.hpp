//
// Created by Giuseppe Francione on 20/09/25.
//

#ifndef MONOLITH_FILE_SCANNER_HPP
#define MONOLITH_FILE_SCANNER_HPP

#include <vector>
#include <filesystem>
#include <random>
#include "../containers/archive_handler.hpp"

void collect_inputs(const std::vector<std::filesystem::path>& inputs,
                    bool recursive,
                    std::vector<std::filesystem::path>& files,
                    std::vector<ContainerJob>& archive_jobs, Settings& settings);

inline std::filesystem::path make_temp_path(const std::filesystem::path &stem, const std::string &ext) {
    thread_local std::mt19937_64 rng{std::random_device{}()};
    thread_local std::uniform_int_distribution<unsigned long long> dist;
    return std::filesystem::temp_directory_path() /
           std::filesystem::path(stem.string() + "_tmp" + std::to_string(dist(rng)) + ext);
}

#endif //MONOLITH_FILE_SCANNER_HPP