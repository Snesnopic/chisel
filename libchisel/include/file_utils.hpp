//
// Created by Giuseppe Francione on 13/11/25.
//

#ifndef CHISEL_FILE_UTILS_HPP
#define CHISEL_FILE_UTILS_HPP

#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

namespace chisel {

    /**
     * @brief Opens a file using a filesystem path, handling Windows Unicode correctly.
     * @param path The path to the file.
     * @param mode The standard C fopen mode string (e.g., "rb", "wb").
     * @return FILE* pointer or nullptr if open failed.
     */
    FILE *open_file(const std::filesystem::path &path, const char *mode);

    /**
     * @brief Creates a unique temporary directory for processing.
     *
     * Creates a directory inside the system temp path using a
     * "chisel-{prefix}-{filename_stem}_{random_suffix}" pattern.
     *
     * @param input_path The input file path (used for its stem).
     * @param prefix A short prefix (e.g., "flac", "pdf").
     * @return Filesystem path to the newly created temporary directory.
     */
    std::filesystem::path make_temp_dir_for(const std::filesystem::path &input_path,
                                            const std::string &prefix);

    /**
     * @brief Recursively removes a directory and logs any errors.
     * @param dir The path to the directory to be removed.
     * @param tag The logger tag (e.g., "flac_processor").
     */
    void cleanup_temp_dir(const std::filesystem::path &dir,
                          std::string_view tag = "file_utils");
} // namespace chisel

#endif // CHISEL_FILE_UTILS_HPP
