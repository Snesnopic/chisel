//
// Created by Giuseppe Francione on 19/10/25.
//

/**
 * @file odf_processor.hpp
 * @brief Defines the IProcessor implementation for OpenDocument Format (ODF) files.
 */

#ifndef CHISEL_ODF_PROCESSOR_HPP
#define CHISEL_ODF_PROCESSOR_HPP

#include "processor.hpp"
#include <array>
#include <string_view>
#include <span>

namespace chisel {

    /**
     * @brief Implements IProcessor for OpenDocument Format (ODF) files.
     *
     * @details This processor handles .odt, .ods, .odp, and .odg files.
     * It treats them as ZIP archives, extracts their contents,
     * and re-compresses embedded XML files (e.g., content.xml)
     * using Zopfli during the finalization phase.
     */
    class OdfProcessor final : public IProcessor {
    public:
        // --- self-description ---
        [[nodiscard]] std::string_view get_name() const noexcept override {
            return "OdfProcessor";
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_mime_types() const noexcept override {
            static constexpr std::array<std::string_view, 4> kMimes = {
                "application/vnd.oasis.opendocument.text",
                "application/vnd.oasis.opendocument.spreadsheet",
                "application/vnd.oasis.opendocument.presentation",
                "application/vnd.oasis.opendocument.graphics"
            };
            return {kMimes.data(), kMimes.size()};
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_extensions() const noexcept override {
            static constexpr std::array<std::string_view, 4> kExts = { ".odt", ".ods", ".odp", ".odg" };
            return {kExts.data(), kExts.size()};
        }

        // --- capabilities ---

        /**
         * @brief Direct recompression is not supported (handled via extraction).
         * @return false
         */
        [[nodiscard]] bool can_recompress() const noexcept override { return false; }

        /**
         * @brief This processor extracts files from ODF archives.
         * @return true
         */
        [[nodiscard]] bool can_extract_contents() const noexcept override { return true; }

        // --- operations ---

        /**
         * @brief (Not Implemented) Direct recompression is not supported.
         */
        void recompress(const std::filesystem::path&,
                        const std::filesystem::path&,
                        bool) override {}

        /**
         * @brief Extracts all files from the ODF (ZIP) container.
         *
         * Uses libarchive to unzip the .odt/.ods/etc. file into
         * a temporary directory.
         *
         * @param input_path Path to the ODF file.
         * @return An ExtractedContent struct.
         */
        std::optional<ExtractedContent> prepare_extraction(
            const std::filesystem::path& input_path) override;

        /**
         * @brief Rebuilds the ODF archive with optimized XML.
         *
         * Iterates through the extracted files. The `mimetype` file is
         * stored uncompressed (as required by the ODF standard).
         * Embedded XML files are recompressed using Zopfli.
         * All other files (e.g., images) are re-added using standard Deflate.
         *
         * @param content The ExtractedContent struct from `prepare_extraction`.
         * @param target_format (Ignored) This processor always writes a ZIP.
         * @return Path to the newly created temporary ODF file.
         * @throws std::runtime_error if archive creation fails.
         */
        std::filesystem::path finalize_extraction(const ExtractedContent &content,
                                                  ContainerFormat target_format) override;

        // --- integrity check ---

        /**
         * @brief (Not Implemented) Compute a raw checksum.
         * @param file_path Path to the file.
         * @return An empty string.
         */
        [[nodiscard]] std::string get_raw_checksum(const std::filesystem::path& file_path) const override;
    };

} // namespace chisel

#endif // CHISEL_ODF_PROCESSOR_HPP