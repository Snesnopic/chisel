//
// Created by Giuseppe Francione on 18/11/25.
//

#ifndef CHISEL_OGG_PROCESSOR_HPP
#define CHISEL_OGG_PROCESSOR_HPP

#include "processor.hpp"
#include <array>
#include <string_view>
#include <span>

namespace chisel {

    /**
     * @brief Implements IProcessor for Ogg (Vorbis/Opus/FLAC) files.
     *
     * @details Supports two main operations:
     * 1. Recompression of Ogg FLAC streams (lossless audio optimization).
     * 2. Extraction and optimization of embedded cover art for all Ogg variants
     * (Vorbis, Opus, FLAC) via TagLib.
     */
    class OggProcessor final : public IProcessor {
    public:
        // self-description
        [[nodiscard]] std::string_view get_name() const noexcept override {
            return "OggProcessor";
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_mime_types() const noexcept override {
            static constexpr std::array<std::string_view, 3> kMimes = {
                "audio/ogg", "audio/vorbis", "audio/opus"
            };
            return {kMimes.data(), kMimes.size()};
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_extensions() const noexcept override {
            static constexpr std::array<std::string_view, 3> kExts = { ".ogg", ".opus", ".oga" };
            return {kExts.data(), kExts.size()};
        }

        // capabilities
        [[nodiscard]] bool can_recompress() const noexcept override { return true; }
        [[nodiscard]] bool can_extract_contents() const noexcept override { return true; }

        // operations

        /**
         * @brief Attempts to recompress Ogg-FLAC streams.
         *
         * If the input is Ogg FLAC, decodes and re-encodes with maximum compression,
         * preserving Vorbis comments. If input is lossy (Vorbis/Opus), performs a
         * direct copy to preserve data.
         */
        void recompress(const std::filesystem::path& input,
                        const std::filesystem::path& output,
                        bool preserve_metadata) override;

        /**
         * @brief Extracts cover art (METADATA_BLOCK_PICTURE) using TagLib.
         */
        std::optional<ExtractedContent> prepare_extraction(
            const std::filesystem::path& input_path) override;

        /**
         * @brief Re-inserts optimized cover art into the processed file.
         */
        std::filesystem::path finalize_extraction(const ExtractedContent &content,
                                                  ContainerFormat target_format) override;

        // integrity check
        [[nodiscard]] std::string get_raw_checksum(const std::filesystem::path& file_path) const override { return ""; }
    };

} // namespace chisel

#endif // CHISEL_OGG_PROCESSOR_HPP