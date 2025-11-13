//
// Created by Giuseppe Francione on 19/10/25.
//

/**
 * @file sqlite_processor.hpp
 * @brief Defines the IProcessor implementation for SQLite database files.
 */

#ifndef CHISEL_SQLITE_PROCESSOR_HPP
#define CHISEL_SQLITE_PROCESSOR_HPP

#include "processor.hpp"
#include <array>
#include <string_view>
#include <span>

namespace chisel {

    /**
     * @brief Implements IProcessor for SQLite database files using sqlite3.
     *
     * @details This processor optimizes SQLite databases by performing
     * a `VACUUM` and `ANALYZE` operation, which rebuilds the
     * database file and updates statistics, potentially reducing file size.
     */
    class SqliteProcessor final : public IProcessor {
    public:
        // --- self-description ---
        [[nodiscard]] std::string_view get_name() const noexcept override {
            return "SqliteProcessor";
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_mime_types() const noexcept override {
            static constexpr std::array<std::string_view, 1> kMimes = { "application/x-sqlite3" };
            return {kMimes.data(), kMimes.size()};
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_extensions() const noexcept override {
            static constexpr std::array<std::string_view, 3> kExts = { ".sqlite", ".db", ".sqlite3" };
            return {kExts.data(), kExts.size()};
        }

        // --- capabilities ---
        [[nodiscard]] bool can_recompress() const noexcept override { return true; }
        [[nodiscard]] bool can_extract_contents() const noexcept override { return false; }

        // --- operations ---

        /**
         * @brief Recompresses (optimizes) an SQLite database file.
         *
         * First, copies the input file to the output path.
         * Then, opens the output file and executes the `VACUUM;`
         * and `ANALYZE;` commands to rebuild the file and optimize indices.
         *
         * @param input Path to the source SQLite file.
         * @param output Path to write the optimized SQLite file.
         * @param preserve_metadata (Ignored) Metadata is inherently
         * part of the database file.
         * @throws std::runtime_error if sqlite3 fails to open or execute commands.
         */
        void recompress(const std::filesystem::path& input,
                        const std::filesystem::path& output,
                        bool preserve_metadata) override;

        /**
         * @brief SQLite is not treated as a container format.
         * @return std::nullopt
         */
        std::optional<ExtractedContent> prepare_extraction(
            [[maybe_unused]] const std::filesystem::path& input_path) override { return std::nullopt; }

        /**
         * @brief SQLite is not treated as a container format.
         * @return Empty path.
         */
        std::filesystem::path finalize_extraction(const ExtractedContent &,
                                                  [[maybe_unused]] ContainerFormat target_format) override {return {};}

        // --- integrity check ---

        /**
         * @brief (Not Implemented) Compute a raw checksum.
         * @param file_path Path to the file.
         * @return An empty string.
         */
        [[nodiscard]] std::string get_raw_checksum(const std::filesystem::path& file_path) const override;
    };

} // namespace chisel

#endif // CHISEL_SQLITE_PROCESSOR_HPP