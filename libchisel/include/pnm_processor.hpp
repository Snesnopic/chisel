//
// Created by Giuseppe Francione on 20/11/25.
//

/**
 * @file pnm_processor.hpp
 * @brief Defines the IProcessor implementation for PNM (PPM/PGM) image files.
 */

#ifndef CHISEL_PNM_PROCESSOR_HPP
#define CHISEL_PNM_PROCESSOR_HPP

#include "processor.hpp"
#include <array>
#include <string_view>
#include <span>

namespace chisel {

    /**
     * @brief Implements IProcessor for PNM files (PPM, PGM, PNM).
     *
     * @details This processor optimizes PNM files by ensuring they are stored
     * in the binary ("raw") format (P5 for grayscale, P6 for RGB), which is
     * significantly smaller than the ASCII format (P2/P3).
     * Reading is handled by stb_image, writing by a custom internal implementation.
     */
    class PnmProcessor final : public IProcessor {
    public:
        // --- self-description ---
        [[nodiscard]] std::string_view get_name() const noexcept override {
            return "PnmProcessor";
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_mime_types() const noexcept override {
            static constexpr std::array<std::string_view, 2> kMimes = {
                "image/x-portable-anymap", "image/x-portable-pixmap"
            };
            return {kMimes.data(), kMimes.size()};
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_extensions() const noexcept override {
            static constexpr std::array<std::string_view, 3> kExts = { ".ppm", ".pgm", ".pnm" };
            return {kExts.data(), kExts.size()};
        }

        // --- capabilities ---
        [[nodiscard]] bool can_recompress() const noexcept override { return true; }
        [[nodiscard]] bool can_extract_contents() const noexcept override { return false; }

        // --- operations ---

        /**
         * @brief Recompresses a PNM file to its binary representation.
         *
         * Reads the image using stb_image. If it's grayscale, writes a P5 (Binary PGM).
         * If it's color, writes a P6 (Binary PPM). Alpha channels are dropped as standard
         * PNM doesn't support them.
         *
         * @param input Path to the source PNM file.
         * @param output Path to write the optimized PNM file.
         * @param preserve_metadata Ignored (PNM has no metadata standard).
         */
        void recompress(const std::filesystem::path& input,
                        const std::filesystem::path& output,
                        bool preserve_metadata) override;

        std::optional<ExtractedContent> prepare_extraction(
            [[maybe_unused]] const std::filesystem::path& input_path) override { return std::nullopt; }

        std::filesystem::path finalize_extraction(const ExtractedContent &) override { return {}; }

        // --- integrity check ---
        [[nodiscard]] std::string get_raw_checksum(const std::filesystem::path& file_path) const override;

        [[nodiscard]] bool raw_equal(const std::filesystem::path &a, const std::filesystem::path &b) const override;
    };

} // namespace chisel

#endif // CHISEL_PNM_PROCESSOR_HPP