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
     * files to be processed by image optimizers.
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
         * @brief This processor can recompress with mp3packercpp.
         * @return true
         */
        [[nodiscard]] bool can_recompress() const noexcept override {
            return true;
        }

        /**
         * @brief This processor extracts cover art.
         * @return true
         */
        [[nodiscard]] bool can_extract_contents() const noexcept override { return true; }

        // --- operations ---

        /**
         * @brief Performs lossless MP3 recompression via mp3ackercpp.
         * @throws std::runtime_error On compression failure or IO error.
         */
        void recompress(const std::filesystem::path& input,
                        const std::filesystem::path& output, const ProcessingOptions &options) override;

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
         * @param options Processing options (e.g. metadata preservation).
         * @return Path to the finalized MP3 file.
         */
        std::filesystem::path finalize_extraction(const ExtractedContent &content, const ProcessingOptions &options) override;

        // --- integrity check ---
        [[nodiscard]] std::string get_raw_checksum(const std::filesystem::path& file_path) const override { return ""; }

        /**
         * @brief Compares two MP3 files by decoding them to raw PCM (via
         * mp3packercpp's minimp3-based decoder) and comparing.
         * @param a First MP3 file.
         * @param b Second MP3 file.
         * @return true if the decoded PCM data and audio parameters are identical.
         */
        [[nodiscard]] bool raw_equal(const std::filesystem::path &a, const std::filesystem::path &b) const override;
    };

} // namespace chisel

#endif // CHISEL_MPEG_PROCESSOR_HPP