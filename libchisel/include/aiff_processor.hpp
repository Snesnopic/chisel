//
// Created by Giuseppe Francione on 19/11/25.
//

/**
 * @file aiff_processor.hpp
 * @brief Defines the IProcessor implementation for AIFF audio files.
 */

#ifndef CHISEL_AIFF_PROCESSOR_HPP
#define CHISEL_AIFF_PROCESSOR_HPP

#include "processor.hpp"
#include <array>
#include <string_view>
#include <span>

namespace chisel {

    /**
     * @brief Implements IProcessor for AIFF files.
     *
     * @details This processor acts as a container for extracting and
     * re-inserting cover art (via ID3v2 tags embedded in the 'ID3 ' chunk).
     */
    class AiffProcessor final : public IProcessor {
    public:
        // --- self-description ---
        [[nodiscard]] std::string_view get_name() const noexcept override {
            return "AiffProcessor";
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_mime_types() const noexcept override {
            static constexpr std::array<std::string_view, 2> kMimes = {
                "audio/x-aiff", "audio/aiff"
            };
            return {kMimes.data(), kMimes.size()};
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_extensions() const noexcept override {
            static constexpr std::array<std::string_view, 3> kExts = { ".aif", ".aiff", ".aifc" };
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

#endif // CHISEL_AIFF_PROCESSOR_HPP