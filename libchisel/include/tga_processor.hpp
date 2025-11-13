//
// Created by Giuseppe Francione on 12/11/25.
//

/**
 * @file tga_processor.hpp
 * @brief Defines the IProcessor implementation for TGA (Targa) image files.
 */

#ifndef CHISEL_TGA_PROCESSOR_HPP
#define CHISEL_TGA_PROCESSOR_HPP

#include "processor.hpp"
#include <array>
#include <string_view>
#include <span>

namespace chisel {

    /**
     * @brief Implements IProcessor for TGA files using stb_image.h and stb_image_write.h.
     *
     * @details This processor's main goal is to reduce file size by
     * ensuring RLE (Run-Length Encoding) is applied. It decodes
     * the TGA and re-encodes it with RLE enabled.
     */
    class TgaProcessor final : public IProcessor {
    public:
        // --- self-description ---
        [[nodiscard]] std::string_view get_name() const noexcept override {
            return "TgaProcessor";
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_mime_types() const noexcept override {
            static constexpr std::array<std::string_view, 2> kMimes = { "image/tga", "image/x-tga" };
            return {kMimes.data(), kMimes.size()};
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_extensions() const noexcept override {
            static constexpr std::array<std::string_view, 2> kExts = { ".tga", ".targa" };
            return {kExts.data(), kExts.size()};
        }

        // --- capabilities ---
        [[nodiscard]] bool can_recompress() const noexcept override { return true; }
        [[nodiscard]] bool can_extract_contents() const noexcept override { return false; }

        // --- operations ---

        /**
         * @brief Recompresses a TGA file by re-applying RLE.
         *
         * Uses `stbi_load` to decode the image and `stbi_write_tga`
         * with `stbi_write_tga_with_rle = 1` to re-encode it
         * with Run-Length Encoding.
         *
         * @param input Path to the source TGA file.
         * @param output Path to write the optimized TGA file.
         * @param preserve_metadata TGA metadata is not preserved by this processor.
         * @throws std::runtime_error if stb_image fails to load or write.
         */
        void recompress(const std::filesystem::path& input,
                        const std::filesystem::path& output,
                        bool preserve_metadata) override;

        /**
         * @brief TGA is not a container format.
         * @return std::nullopt
         */
        std::optional<ExtractedContent> prepare_extraction(
            [[maybe_unused]] const std::filesystem::path& input_path) override { return std::nullopt; }

        /**
         * @brief TGA is not a container format.
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
         * @brief Compares two TGA files by decoding them to raw RGBA8 and comparing.
         *
         * Uses `stbi_load` to force decoding to 4 channels (RGBA) for
         * a consistent pixel-by-pixel comparison.
         *
         * @param a First TGA file.
         * @param b Second TGA file.
         * @return true if the decoded pixel data and dimensions are identical.
         */
        [[nodiscard]] bool raw_equal(const std::filesystem::path &a, const std::filesystem::path &b) const override;
    };

} // namespace chisel

#endif // CHISEL_TGA_PROCESSOR_HPP