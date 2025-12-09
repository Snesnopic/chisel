//
// Created by Giuseppe Francione on 20/11/25.
//

/**
 * @file bmp_processor.hpp
 * @brief Defines the IProcessor implementation for BMP image files using rupertwh/bmplib.
 */

#ifndef CHISEL_BMP_PROCESSOR_HPP
#define CHISEL_BMP_PROCESSOR_HPP

#include "processor.hpp"
#include <array>
#include <string_view>
#include <vector>
#include <span>

namespace chisel {

    /**
     * @brief Implements IProcessor for BMP files using bmplib.
     *
     * @details This processor optimizes BMP files by detecting if they utilize
     * a color palette. If so, it attempts to apply RLE (Run-Length Encoding)
     * compression (RLE4 or RLE8 via BMP_RLE_AUTO). For standard RGB images,
     * it rewrites the file ensuring a minimal header structure.
     */
    class BmpProcessor final : public IProcessor {
    public:
        // --- self-description ---
        [[nodiscard]] std::string_view get_name() const noexcept override {
            return "BmpProcessor";
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_mime_types() const noexcept override {
            static constexpr std::array<std::string_view, 2> kMimes = { "image/bmp", "image/x-ms-bmp" };
            return {kMimes.data(), kMimes.size()};
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_extensions() const noexcept override {
            static constexpr std::array<std::string_view, 2> kExts = { ".bmp", ".dib" };
            return {kExts.data(), kExts.size()};
        }

        // --- capabilities ---
        [[nodiscard]] bool can_recompress() const noexcept override { return true; }
        [[nodiscard]] bool can_extract_contents() const noexcept override { return false; }

        // --- operations ---

        /**
         * @brief Recompresses a BMP file.
         *
         * Opens the file using bmplib. If the image has a palette, it loads
         * indices and palette, then saves using BMP_RLE_AUTO to find the
         * best RLE compression. If RGB, it saves standard uncompressed data.
         *
         * @param input Path to the source BMP file.
         * @param output Path to write the optimized BMP file.
         * @param preserve_metadata BMP headers contain minimal metadata which is preserved;
         * auxiliary data at EOF is currently dropped.
         * @throws std::runtime_error if reading or writing fails.
         */
        void recompress(const std::filesystem::path& input,
                        const std::filesystem::path& output,
                        bool preserve_metadata) override;

        /**
         * @brief BMP is not a container format.
         * @return std::nullopt
         */
        std::optional<ExtractedContent> prepare_extraction(
            [[maybe_unused]] const std::filesystem::path& input_path) override { return std::nullopt; }

        /**
         * @brief BMP is not a container format.
         * @return Empty path.
         */
        std::filesystem::path finalize_extraction(const ExtractedContent &) override { return {}; }

        // --- integrity check ---

        /**
         * @brief (Not Implemented) Compute a raw checksum.
         * @param file_path Path to the file.
         * @return An empty string.
         */
        [[nodiscard]] std::string get_raw_checksum(const std::filesystem::path& file_path) const override;

        /**
         * @brief Compares two BMP files by decoding pixels.
         *
         * Decodes both files to raw buffers (RGB or Indexed) and compares
         * content and dimensions.
         *
         * @param a First BMP file.
         * @param b Second BMP file.
         * @return true if logical image data matches.
         */
        [[nodiscard]] bool raw_equal(const std::filesystem::path &a, const std::filesystem::path &b) const override;
    };

} // namespace chisel

#endif // CHISEL_BMP_PROCESSOR_HPP