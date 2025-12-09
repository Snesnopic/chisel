//
// Created by Giuseppe Francione on 18/11/25.
//

/**
 * @file mpeg_processor.hpp
 * @brief Defines the IProcessor implementation for MPEG Audio (MP3) files.
 */

#ifndef CHISEL_MPEG_PROCESSOR_HPP
#define CHISEL_MPEG_PROCESSOR_HPP

#include "processor.hpp"
#include <array>
#include <string_view>
#include <span>

namespace chisel {

    /**
     * @brief Implements IProcessor for MPEG Audio (MP3) files.
     *
     * @details This processor acts as a container for extracting and
     * re-inserting cover art (ID3v2 APIC tags), allowing the image
     * files to be processed by image optimizers. Audio recompression
     * logic (Phase 2) is left as a future implementation task.
     */
    class MpegProcessor final : public IProcessor {
    public:
        // --- self-description ---
        [[nodiscard]] std::string_view get_name() const noexcept override {
            return "MpegProcessor";
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_mime_types() const noexcept override {
            static constexpr std::array<std::string_view, 1> kMimes = { "audio/mpeg" };
            return {kMimes.data(), kMimes.size()};
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_extensions() const noexcept override {
            static constexpr std::array<std::string_view, 1> kExts = { ".mp3" };
            return {kExts.data(), kExts.size()};
        }

        // --- capabilities ---
        /**
         * @brief Audio recompression is planned for the future.
         * @return false
         */
        [[nodiscard]] bool can_recompress() const noexcept override { return false; }

        /**
         * @brief This processor extracts cover art.
         * @return true
         */
        [[nodiscard]] bool can_extract_contents() const noexcept override { return true; }

        // --- operations ---

        /**
         * @brief (Placeholder) Direct recompression is not implemented yet.
         *
         * @throws std::runtime_error always, if this is called improperly.
         */
        void recompress(const std::filesystem::path& input,
                        const std::filesystem::path& output,
                        bool preserve_metadata) override;

        /**
         * @brief Extracts embedded cover art (ID3v2 APIC tags) using AudioMetadataUtil.
         * @param input_path Path to the MP3 file.
         * @return ExtractedContent struct with cover art files, or nullopt.
         */
        std::optional<ExtractedContent> prepare_extraction(
            const std::filesystem::path& input_path) override;

        /**
         * @brief Re-inserts optimized cover art into the MP3 file.
         * @param content Content descriptor from prepare_extraction.
         * @param target_format Ignored.
         * @return Path to the finalized MP3 file.
         */
        std::filesystem::path finalize_extraction(const ExtractedContent &content) override;

        // --- integrity check ---
        [[nodiscard]] std::string get_raw_checksum(const std::filesystem::path& file_path) const override { return ""; }
    };

} // namespace chisel

#endif // CHISEL_MPEG_PROCESSOR_HPP