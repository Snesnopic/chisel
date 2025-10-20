//
// Created by Giuseppe Francione on 20/09/25.
//

#ifndef CHISEL_FILE_SCANNER_HPP
#define CHISEL_FILE_SCANNER_HPP

#include <vector>
#include <filesystem>

std::vector<std::filesystem::path>
collect_input_files(const std::vector<std::filesystem::path>& inputs,
                    bool recursive,
                    bool& is_pipe);

#endif //CHISEL_FILE_SCANNER_HPP