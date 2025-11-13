//
// Created by Giuseppe Francione on 19/10/25.
//

/**
 * @file zopflipng_processor.hpp
 * @brief Defines the IProcessor implementation for PNG files using ZopfliPNG.
 */

#ifndef CHISEL_ZOPFLIPNG_PROCESSOR_HPP
#define CHISEL_ZOPFLIPNG_PROCESSOR_HPP

#include "processor.hpp"
#include <array>
#include <string_view>
#include <span>

namespace chisel {

    /**
     * @brief Implements an aggressive IProcessor for PNGs using ZopfliPNG.
     *
     * @details This processor uses Google's Zopfli compression algorithm
     * to re-compress PNG IDAT streams. It is significantly slower
     * than the default PngProcessor but may achieve better compression.
     * It is intended to be run in a pipeline (e.g., after PngProcessor).
     */
    class ZopfliPngProcessor final : public IProcessor {
    public:
        // --- self-description ---
        [[nodiscard]] std::string_view get_name() const noexcept override {
            return "ZopflipngProcessor";
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_mime_types() const noexcept override {
            static constexpr std::array<std::string_view, 1> kMimes = { "image/png" };
            return {kMimes.data(), kMimes.size()};
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_extensions() const noexcept override {
            static constexpr std::array<std::string_view, 1> kExts = { ".png" };
            return {kExts.data(), kExts.size()};
        }

        // --- capabilities ---
        [[nodiscard]] bool can_recompress() const noexcept override { return true; }
        [[nodiscard]] bool can_extract_contents() const noexcept override { return false; }

        // --- operations ---

        /**
         * @brief Optimizes a PNG file using ZopfliPNG.
         *
         * Uses the `ZopfliPNGOptimize` function to re-compress the file
         * with a specified number of iterations (e.g., 15).
         *
         * @param input Path to the source PNG file.
         * @param output Path to write the optimized PNG file.
         * @param preserve_metadata If true, attempts to keep common
         * text and ancillary chunks.
         * @throws std::runtime_error if optimization fails.
         */
        void recompress(const std::filesystem::path& input,
                        const std::filesystem::path& output,
                        bool preserve_metadata) override;

        /**
         * @brief This format cannot be extracted.
         * @return std::nullopt
         */
        std::optional<ExtractedContent> prepare_extraction(
            [[maybe_unused]] const std::filesystem::path& input_path) override { return std::nullopt; }

        /**
         * @brief This format cannot be extracted.
         * @return Empty path.
         */
        std::filesystem::path finalize_extraction(const ExtractedContent &,
                                                  [[maybe_unused]] ContainerFormat target_format) override {return {};}

        // --- integrity check ---

        /**
         * @brief (Not Implemented) Compute a raw checksum.
         * @param file_path Path to the file.
         * @return An empty string.
         */
        [[nodiscard]] std::string get_raw_checksum(const std::filesystem::path& file_path) const override;

        /**
         * @brief (Static Helper) Recompresses a raw zlib data buffer with Zopfli.
         * @param input Raw data to compress.
         * @return A vector containing the Zopfli-compressed zlib stream.
         */
        static std::vector<unsigned char> recompress_with_zopfli(const std::vector<unsigned char>& input);
    };

} // namespace chisel

#endif // CHISEL_ZOPFLIPNG_PROCESSOR_HPP