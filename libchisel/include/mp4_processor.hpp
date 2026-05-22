//
// Created by Giuseppe Francione on 18/11/25.
//

/**
 * @file mp4_processor.hpp
 * @brief Processor for MP4 files.
 */

#ifndef CHISEL_MP4_PROCESSOR_HPP
#define CHISEL_MP4_PROCESSOR_HPP

#include "processor.hpp"
#include <array>
#include <string_view>
#include <span>

namespace chisel {

    /**
     * @brief Implements IProcessor for MP4/M4A files.
     *
     * @details Extracts and re-inserts cover art (atom 'covr') using
     * AudioMetadataUtil. Stream recompression is not yet implemented.
     */
    class Mp4Processor final : public IProcessor {
    public:
        [[nodiscard]] std::string_view get_name() const noexcept override {
            return "Mp4Processor";
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_mime_types() const noexcept override {
            static constexpr std::array<std::string_view, 3> kMimes = {
                "audio/mp4", "audio/x-m4a", "video/mp4"
            };
            return {kMimes.data(), kMimes.size()};
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_extensions() const noexcept override {
            static constexpr std::array<std::string_view, 3> kExts = { ".mp4", ".m4a", ".m4b" };
            return {kExts.data(), kExts.size()};
        }

        [[nodiscard]] bool can_recompress() const noexcept override { return false; }
        [[nodiscard]] bool can_extract_contents() const noexcept override { return true; }

        /**
         * @brief (Not Implemented) Direct recompression is not supported.
         * @param input Path to the source MP4 file.
         * @param output Path to write the optimized MP4 file.
         * @param options Processing options.
         */
        void recompress(const std::filesystem::path& input,
                        const std::filesystem::path& output, const ProcessingOptions &options) override;

        /**
         * @brief Prepares extraction of cover art.
         * @param input_path Path to the MP4 file.
         * @return ExtractedContent struct or nullopt.
         */
        std::optional<ExtractedContent> prepare_extraction(
            const std::filesystem::path& input_path) override;

        /**
         * @brief Rebuilds the MP4 file with optimized cover art.
         * @param content The ExtractedContent struct.
         * @param options Processing options (e.g. metadata preservation).
         * @return Path to the finalized MP4 file.
         */
        std::filesystem::path finalize_extraction(const ExtractedContent &content, const ProcessingOptions &options) override;

        /**
         * @brief (Not Implemented) Compute a raw checksum.
         * @param file_path Path to the file.
         * @return An empty string.
         */
        [[nodiscard]] std::string get_raw_checksum(const std::filesystem::path& file_path) const override { return ""; }
    };

} // namespace chisel

#endif // CHISEL_MP4_PROCESSOR_HPP