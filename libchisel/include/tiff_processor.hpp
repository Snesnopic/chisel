//
// Created by Giuseppe Francione on 19/10/25.
//

/**
 * @file tiff_processor.hpp
 * @brief Defines the IProcessor implementation for TIFF image files.
 */

#ifndef CHISEL_TIFF_PROCESSOR_HPP
#define CHISEL_TIFF_PROCESSOR_HPP

#include "processor.hpp"
#include <array>
#include <string_view>
#include <span>

namespace chisel {

    /**
     * @brief Implements IProcessor for TIFF files using libtiff.
     *
     * @details This processor standardizes TIFF files by decoding them
     * to a raw RGBA8 pixel buffer and re-encoding them using
     * maximum Deflate (Zip) compression.
     */
    class TiffProcessor final : public IProcessor {
    public:
        // --- self-description ---
        [[nodiscard]] std::string_view get_name() const noexcept override {
            return "TiffProcessor";
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_mime_types() const noexcept override {
            static constexpr std::array<std::string_view, 1> kMimes = { "image/tiff" };
            return {kMimes.data(), kMimes.size()};
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_extensions() const noexcept override {
            static constexpr std::array<std::string_view, 2> kExts = { ".tif", ".tiff" };
            return {kExts.data(), kExts.size()};
        }

        // --- capabilities ---
        [[nodiscard]] bool can_recompress() const noexcept override { return true; }
        [[nodiscard]] bool can_extract_contents() const noexcept override { return false; }

        // --- operations ---

        /**
         * @brief Recompresses a TIFF file using libtiff.
         *
         * Reads the input image (handling decompression of various
         * formats) into a standard RGBA8 pixel buffer using
         * `TIFFReadRGBAImageOriented`.
         *
         * It then re-encodes this buffer into a new TIFF using
         * `COMPRESSION_ADOBE_DEFLATE` (Zip) at maximum level (9).
         * This handles multi-page TIFFs by iterating through directories.
         *
         * @param input Path to the source TIFF file.
         * @param output Path to write the optimized TIFF file.
         * @param preserve_metadata If true, copies standard metadata tags
         * (Resolution, ICC, EXIF, XMP) to the new file.
         * @throws std::runtime_error if libtiff encounters a fatal error.
         */
        void recompress(const std::filesystem::path& input,
                        const std::filesystem::path& output,
                        bool preserve_metadata) override;

        /**
         * @brief TIFF is not a container format.
         * @return std::nullopt
         */
        std::optional<ExtractedContent> prepare_extraction(
            [[maybe_unused]] const std::filesystem::path& input_path) override { return std::nullopt; }

        /**
         * @brief TIFF is not a container format.
         * @return Empty path.
         */
        std::filesystem::path finalize_extraction(const ExtractedContent &) override {return {};}

        // --- integrity check ---

        /**
         * @brief (Not Implemented) Compute a raw checksum.
         * @param file_path Path to the file.
         * @return An empty string.
         */
        [[nodiscard]] std::string get_raw_checksum(const std::filesystem::path& file_path) const override;

        /**
         * @brief Compares two TIFF files by decoding them to raw RGBA8 and comparing.
         *
         * Handles multi-page TIFFs by comparing each page sequentially.
         *
         * @param a First TIFF file.
         * @param b Second TIFF file.
         * @return true if the decoded pixel data, dimensions, and page
         * count are identical.
         */
        [[nodiscard]] bool raw_equal(const std::filesystem::path &a, const std::filesystem::path &b) const override;
    };

} // namespace chisel

#endif // CHISEL_TIFF_PROCESSOR_HPP