//
// Created by Giuseppe Francione on 20/09/25.
//

#ifndef MONOLITH_FILE_SCANNER_HPP
#define MONOLITH_FILE_SCANNER_HPP

#include <vector>
#include <filesystem>
#include "../containers/archive_handler.hpp"

void collect_inputs(const std::vector<std::filesystem::path>& inputs,
                    bool recursive,
                    std::vector<std::filesystem::path>& files,
                    std::vector<ContainerJob>& archive_jobs);

#endif //MONOLITH_FILE_SCANNER_HPP