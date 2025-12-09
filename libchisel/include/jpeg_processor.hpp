//
// Created by Giuseppe Francione on 19/10/25.
//

/**
 * @file jpeg_processor.hpp
 * @brief Defines the IProcessor implementation for JPEG files.
 */

#ifndef CHISEL_JPEG_PROCESSOR_HPP
#define CHISEL_JPEG_PROCESSOR_HPP

#include "processor.hpp"
#include <array>
#include <string_view>
#include <span>

namespace chisel {

    /**
     * @brief Implements IProcessor for JPEG files using mozjpeg.
     *
     * @details This processor provides lossless recompression by optimizing
     * Huffman tables, similar to `jpegtran`. It does not perform
     * a full decode and re-encode cycle.
     */
    class JpegProcessor final : public IProcessor {
    public:
        // --- self-description ---
        [[nodiscard]] std::string_view get_name() const noexcept override {
            return "JpegProcessor";
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_mime_types() const noexcept override {
            static constexpr std::array<std::string_view, 1> kMimes = { "image/jpeg" };
            return {kMimes.data(), kMimes.size()};
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_extensions() const noexcept override {
            static constexpr std::array<std::string_view, 3> kExts = { ".jpg", ".jpeg", ".jpe" };
            return {kExts.data(), kExts.size()};
        }

        // --- capabilities ---
        [[nodiscard]] bool can_recompress() const noexcept override { return true; }
        [[nodiscard]] bool can_extract_contents() const noexcept override { return false; }

        // --- operations ---

        /**
         * @brief Losslessly recompresses a JPEG file using mozjpeg.
         *
         * Uses `jpegtran` logic to perform a lossless transformation,
         * optimizing Huffman tables and copying critical parameters.
         *
         * @param input Path to the source JPEG file.
         * @param output Path to write the optimized JPEG file.
         * @param preserve_metadata If true, copies APP markers (EXIF, ICC, XMP)
         * and COM markers. If false, strips them.
         * @throws std::runtime_error if libjpeg encounters a fatal error.
         * @note This operation is lossless regarding image data.
         */
        void recompress(const std::filesystem::path& input,
                        const std::filesystem::path& output,
                        bool preserve_metadata) override;

        /**
         * @brief JPEG is not a container format.
         * @return std::nullopt
         */
        std::optional<ExtractedContent> prepare_extraction(
            [[maybe_unused]] const std::filesystem::path& input_path) override { return std::nullopt; }

        /**
         * @brief JPEG is not a container format.
         * @return Empty path.
         */
        std::filesystem::path finalize_extraction(const ExtractedContent &) override {return {};}

        // --- integrity check ---

        /**
         * @brief Compares two JPEG files pixel by pixel.
         *
         * Decodes both images into a raw pixel buffer (RGB or Grayscale)
         * using libjpeg and compares the buffers and dimensions.
         *
         * @param a Path to the first JPEG file.
         * @param b Path to the second JPEG file.
         * @return true if pixel data and dimensions match, false otherwise.
         */
        [[nodiscard]] bool raw_equal(const std::filesystem::path &a, const std::filesystem::path &b) const override;

        /**
         * @brief (Not Implemented) Compute a raw checksum.
         * @param file_path Path to the file.
         * @return An empty string.
         */
        [[nodiscard]] std::string get_raw_checksum(const std::filesystem::path& file_path) const override;
    };

} // namespace chisel

#endif // CHISEL_JPEG_PROCESSOR_HPP