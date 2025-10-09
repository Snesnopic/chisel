//
// Created by Giuseppe Francione on 20/09/25.
//

#ifndef MONOLITH_FILE_SCANNER_HPP
#define MONOLITH_FILE_SCANNER_HPP

#include <vector>
#include <filesystem>
#include "random_utils.hpp"
#include "../containers/archive_handler.hpp"

void collect_inputs(const std::vector<std::filesystem::path>& inputs,
                    bool recursive,
                    std::vector<std::filesystem::path>& files,
                    std::vector<ContainerJob>& archive_jobs, Settings& settings);

inline std::filesystem::path make_temp_path(const std::filesystem::path &stem, const std::string &ext) {
    return std::filesystem::temp_directory_path() /
           std::filesystem::path(stem.string() + "_tmp" + RandomUtils::random_suffix() + ext);
}
std::unique_ptr<IContainer> make_handler(ContainerFormat fmt);

#endif //MONOLITH_FILE_SCANNER_HPP