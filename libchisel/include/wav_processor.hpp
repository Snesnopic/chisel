//
// Created by Giuseppe Francione on 18/11/25.
//

/**
 * @file wav_processor.hpp
 * @brief Defines the IProcessor implementation for WAV audio files.
 */

#ifndef CHISEL_WAV_PROCESSOR_HPP
#define CHISEL_WAV_PROCESSOR_HPP

#include "processor.hpp"
#include <array>
#include <string_view>
#include <span>

namespace chisel {

    /**
     * @brief Implements IProcessor for WAV files.
     *
     * @details This processor currently acts as a container for extracting and
     * re-inserting cover art (via ID3v2 tags embedded in RIFF chunks).
     */
    class WavProcessor final : public IProcessor {
    public:
        // --- self-description ---
        [[nodiscard]] std::string_view get_name() const noexcept override {
            return "WavProcessor";
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_mime_types() const noexcept override {
            static constexpr std::array<std::string_view, 4> kMimes = {
                "audio/wav", "audio/x-wav", "audio/vnd.wave", "audio/wave"
            };
            return {kMimes.data(), kMimes.size()};
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_extensions() const noexcept override {
            static constexpr std::array<std::string_view, 1> kExts = { ".wav" };
            return {kExts.data(), kExts.size()};
        }

        // --- capabilities ---
        [[nodiscard]] bool can_recompress() const noexcept override { return false; }
        [[nodiscard]] bool can_extract_contents() const noexcept override { return true; }

        // --- operations ---

        void recompress(const std::filesystem::path& input,
                        const std::filesystem::path& output,
                        bool preserve_metadata) override;

        std::optional<ExtractedContent> prepare_extraction(
            const std::filesystem::path& input_path) override;

        std::filesystem::path finalize_extraction(const ExtractedContent &content) override;

        // --- integrity check ---
        [[nodiscard]] std::string get_raw_checksum(const std::filesystem::path& file_path) const override { return ""; }
    };

} // namespace chisel

#endif // CHISEL_WAV_PROCESSOR_HPP