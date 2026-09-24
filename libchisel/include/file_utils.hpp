//
// Created by Giuseppe Francione on 13/11/25.
//

/**
 * @file file_utils.hpp
 * @brief Utility definitions for FILE_UTILS.
 */

#ifndef CHISEL_FILE_UTILS_HPP
#define CHISEL_FILE_UTILS_HPP

#include <cstdio>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#include <vector>
#include <cstdint>
#include <fstream>
#include <memory>

namespace chisel {
    /**
     * @brief RAII wrapper for FILE pointers to ensure they are closed.
     */
    struct FileCloser {
        void operator()(FILE *f) const { if (f) std::fclose(f); }
    };
    using unique_FILE = std::unique_ptr<FILE, FileCloser>;

    uint16_t read_le16(const uint8_t* p);
    uint32_t read_le32(const uint8_t* p);
    uint64_t read_le64(const uint8_t* p);

    void write_le16(uint8_t* p, uint16_t v);
    void write_le32(uint8_t* p, uint32_t v);
    void write_le64(uint8_t* p, uint64_t v);

    uint16_t read_be16(const uint8_t* p);
    uint32_t read_be32(const uint8_t* p);
    uint64_t read_be64(const uint8_t* p);

    void write_be16(uint8_t* p, uint16_t v);
    void write_be32(uint8_t* p, uint32_t v);
    void write_be64(uint8_t* p, uint64_t v);

    // padding to 4-byte boundaries
    uint32_t align4(uint32_t v);

    // format zero-padded index for sorting
    std::string format_index(size_t index);
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
     * @brief Sanitizes an archive entry path to prevent directory traversal (zip-slip).
     * @param entry_name The raw entry name from the archive.
     * @param dest_dir The extraction destination.
     * @param out_path [Output] The resulting safe absolute path.
     * @return True if safe, false if malicious or invalid.
     */
    bool sanitize_archive_entry_path(const std::string& entry_name, const std::filesystem::path& dest_dir, std::filesystem::path& out_path);

    /**
     * @brief Checks whether a lexically-normalized path is base, or actually inside it.
     *
     * A plain string-prefix check (e.g. "starts_with") would incorrectly accept a
     * sibling directory that merely shares a prefix with base (e.g. "/tmp/extract-evil"
     * vs "/tmp/extract"); this additionally requires a path separator (or exact
     * equality) at the boundary.
     *
     * @param normalized The lexically-normalized candidate path.
     * @param base The lexically-normalized containing directory.
     * @return True if normalized is base or a genuine descendant of it.
     */
    bool path_is_within(const std::filesystem::path& normalized, const std::filesystem::path& base);

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
     * @brief Writes a block of bytes to a file, replacing it.
     * @return false unless every byte got written and the file was closed without errors.
     */
    bool write_file(const std::filesystem::path& path, const void* data, std::size_t size);

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

    /**
     * @brief Checks whether a file contains a byte sequence, reading it in chunks.
     * @return false if the file can't be read.
     */
    bool file_contains(const std::filesystem::path& path, std::string_view needle);

    /**
     * @brief Checks whether the first limit bytes of a file contain any of some byte sequences.
     * @return false if the file can't be read.
     */
    bool file_contains(const std::filesystem::path& path, std::initializer_list<std::string_view> needles,
                       std::uint64_t limit = std::numeric_limits<std::uint64_t>::max());

} // namespace chisel

#endif // CHISEL_FILE_UTILS_HPP
