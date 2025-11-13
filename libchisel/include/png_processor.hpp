//
// Created by Giuseppe Francione on 19/10/25.
//

/**
 * @file png_processor.hpp
 * @brief Defines the IProcessor implementation for PNG files using libpng.
 */

#ifndef CHISEL_PNG_PROCESSOR_HPP
#define CHISEL_PNG_PROCESSOR_HPP

#include "processor.hpp"
#include <array>
#include <string_view>
#include <span>

namespace chisel {

    /**
     * @brief Implements IProcessor for PNG files using libpng.
     *
     * @details This processor performs a full decode-to-RGBA8, analysis,
     * and re-encode cycle. It intelligently selects the optimal
     * color type (Palette, Grayscale, RGB, etc.) for the given
     * pixel data to maximize compression.
     */
    class PngProcessor final : public IProcessor {
    public:
        // --- self-description ---
        [[nodiscard]] std::string_view get_name() const noexcept override {
            return "PngProcessor";
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_mime_types() const noexcept override {
            static constexpr std::array<std::string_view, 2> kMimes = { "image/png" };
            return {kMimes.data(), kMimes.size()};
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_extensions() const noexcept override {
            static constexpr std::array<std::string_view, 1> kExts = { ".png" };
            return {kExts.data(), kExts.size()};
        }

        // --- capabilities ---
        [[nodiscard]] bool can_recompress() const noexcept override { return true; }
        [[nodiscard]] bool can_extract_contents() const noexcept override { return false; }

        /**
         * @brief This format cannot be extracted.
         * @return std::nullopt
         */
        std::optional<ExtractedContent> prepare_extraction(
            [[maybe_unused]]const std::filesystem::path& input_path) override
        {
            return std::nullopt;
        }

        /**
         * @brief This format cannot be extracted.
         * @return Empty path.
         */
        std::filesystem::path finalize_extraction(const ExtractedContent &,
                                                  [[maybe_unused]] ContainerFormat target_format) override {return {};}

        // --- operations ---

        /**
         * @brief Recompresses a PNG file by decoding and re-encoding.
         *
         * Decodes the image to raw 32-bit RGBA, analyzes the pixel data
         * to find the most optimal color type (e.g., PALETTE if < 256 colors,
         * GRAY if all channels are equal), and then re-encodes
         * using libpng with maximum compression (level 9).
         *
         * @param input Path to the source PNG file.
         * @param output Path to write the optimized PNG file.
         * @param preserve_metadata If true, copies common metadata chunks
         * (iCCP, sRGB, gAMA, tEXt, etc.) to the new file.
         * @throws std::runtime_error if libpng encounters a fatal error.
         */
        void recompress(const std::filesystem::path& input,
                        const std::filesystem::path& output,
                        bool preserve_metadata) override;

        // --- integrity check ---

        /**
         * @brief (Not Implemented) Compute a raw checksum.
         * @param file_path Path to the file.
         * @return An empty string.
         */
        [[nodiscard]] std::string get_raw_checksum(const std::filesystem::path& file_path) const override;

        /**
         * @brief Compares two PNG files by decoding them to raw RGBA8 and comparing.
         *
         * @param a First PNG file.
         * @param b Second PNG file.
         * @return true if the decoded pixel data and dimensions are identical.
         */
        [[nodiscard]] bool raw_equal(const std::filesystem::path &a, const std::filesystem::path &b) const override;
    };

} // namespace chisel

#endif // CHISEL_PNG_PROCESSOR_HPP