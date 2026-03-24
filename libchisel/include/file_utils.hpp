//
// Created by Giuseppe Francione on 13/11/25.
//

#ifndef CHISEL_FILE_UTILS_HPP
#define CHISEL_FILE_UTILS_HPP

#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include <cstdint>
#include <fstream>

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

    /**
     * @brief Converts a string to lowercase copy.
     * @param s The input string.
     * @return Lowercase version of the string.
     */
    std::string to_lower_copy(std::string s);

    /**
     * @brief Ensures that the parent directory for a given path exists.
     * @param p The path for which the parent directory should exist.
     * @param ec Output error code.
     * @return True if successful or directory already exists.
     */
    bool ensure_parent_dirs(const std::filesystem::path& p, std::error_code& ec);

    /**
     * @brief Calculates the relative path of a file with respect to a root directory.
     * Returns filename if relative path cannot be computed.
     * @param root The root directory.
     * @param p The target path.
     * @return Relative path string or filename.
     */
    std::string rel_path_of(const std::filesystem::path& root, const std::filesystem::path& p);

    /**
     * @brief Performs a "natural" string comparison (e.g. file2.txt < file10.txt).
     * @param sa First string.
     * @param sb Second string.
     * @return True if sa is naturally less than sb.
     */
    bool natural_less_string(const std::string& sa, const std::string& sb);

    /**
     * @brief Performs a "natural" path comparison based on relative paths from a root.
     * @param a First path.
     * @param b Second path.
     * @param root Common root directory.
     * @return True if a is naturally less than b relative to root.
     */
    bool natural_less_path(const std::filesystem::path& a, const std::filesystem::path& b, const std::filesystem::path& root);

    /**
     * @brief Sanitizes an archive entry path to prevent directory traversal (zip-slip).
     * @param entry_name The raw entry name from the archive.
     * @param dest_dir The extraction destination.
     * @param out_path [Output] The resulting safe absolute path.
     * @return True if safe, false if malicious or invalid.
     */
    bool sanitize_archive_entry_path(const std::string& entry_name, const std::filesystem::path& dest_dir, std::filesystem::path& out_path);

    /**
     * @brief Writes a byte buffer to a file.
     *
     * Overwrites the file if it already exists. Creates parent directories
     * if necessary. Returns false on I/O errors or if the file cannot be written.
     *
     * @param path The destination file path.
     * @param buf The buffer containing the bytes to write.
     * @return True if the file was successfully written, false otherwise.
     */
    bool write_file(const std::filesystem::path& path, const std::vector<uint8_t>& buf);

    /**
     * @brief Reads the entire contents of a file into a byte buffer.
     *
     * This overload writes the file's raw bytes into the provided output vector.
     * It returns false on I/O errors or if the file cannot be opened.
     *
     * @param path The path to the file to be read.
     * @param buf [Output] The vector that will receive the file's bytes.
     * @return True if the file was successfully read, false otherwise.
     */
    bool read_file(const std::filesystem::path &path, std::vector<uint8_t> &buf);

    /**
     * @brief Reads the entire contents of a file into a byte vector.
     * @param path The path to the file to be read.
     * @return A vector containing the file's raw bytes.
     */
    std::vector<uint8_t> read_file(const std::filesystem::path& path);

} // namespace chisel

#endif // CHISEL_FILE_UTILS_HPP
