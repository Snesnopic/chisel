//
// Created by Giuseppe Francione on 19/10/25.
//

/**
 * @file ape_processor.hpp
 * @brief Defines the IProcessor implementation for Monkey's Audio (APE).
 */

#ifndef CHISEL_APE_PROCESSOR_HPP
#define CHISEL_APE_PROCESSOR_HPP

#include "processor.hpp"
#include <array>
#include <string_view>
#include <span>

namespace chisel {

    /**
     * @brief Implements IProcessor for Monkey's Audio (APE) files using MACLib.
     *
     * Performs a full decode and re-encode cycle to the highest
     * compression level ("insane"). Also attempts to preserve APEv2 tags.
     */
    class ApeProcessor final : public IProcessor {
    public:
        // --- self-description ---
        [[nodiscard]] std::string_view get_name() const noexcept override {
            return "ApeProcessor";
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_mime_types() const noexcept override {
            static constexpr std::array<std::string_view, 2> kMimes = { "audio/ape", "audio/x-ape" };
            return {kMimes.data(), kMimes.size()};
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_extensions() const noexcept override {
            static constexpr std::array<std::string_view, 1> kExts = { ".ape" };
            return {kExts.data(), kExts.size()};
        }

        // --- capabilities ---
        [[nodiscard]] bool can_recompress() const noexcept override { return true; }

        /**
         * @brief This processor extracts cover art using AudioMetadataUtil.
         * @return true
         */
        [[nodiscard]] bool can_extract_contents() const noexcept override { return true; }

        // --- operations ---

        /**
         * @brief Recompresses an APE file using MACLib.
         *
         * Performs a full decode and re-encode cycle using the
         * "insane" compression level (APE_COMPRESSION_LEVEL_INSANE).
         *
         * @param input Path to the source APE file.
         * @param output Path to write the optimized APE file.
         * @param preserve_metadata If true, attempts to copy APEv2 tags.
         * @throws std::runtime_error if the MACLib encoder or decoder fails.
         */
        void recompress(const std::filesystem::path& input,
                        const std::filesystem::path& output,
                        bool preserve_metadata) override;

        /**
         * @brief Extracts embedded cover art from the APE file.
         * @param input_path Path to the APE file.
         * @return ExtractedContent struct with cover art files, or nullopt.
         */
        std::optional<ExtractedContent> prepare_extraction(
            const std::filesystem::path& input_path) override;

        /**
         * @brief Re-inserts optimized cover art into the APE file.
         * @param content Content descriptor from prepare_extraction.
         * @param target_format Ignored.
         * @return Path to the finalized APE file.
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

        /**
         * @brief Compares two APE files by decoding them to raw PCM and comparing.
         *
         * @param a First APE file.
         * @param b Second APE file.
         * @return true if the decoded PCM data and audio parameters are identical.
         */
        [[nodiscard]] bool raw_equal(const std::filesystem::path &a, const std::filesystem::path &b) const override;
    };

} // namespace chisel

#endif // CHISEL_APE_PROCESSOR_HPP