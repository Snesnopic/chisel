//
// Created by Giuseppe Francione on 19/10/25.
//

/**
 * @file webp_processor.hpp
 * @brief Defines the IProcessor implementation for WebP image files.
 */

#ifndef CHISEL_WEBP_PROCESSOR_HPP
#define CHISEL_WEBP_PROCESSOR_HPP

#include "processor.hpp"
#include <array>
#include <string_view>
#include <span>

namespace chisel {

    /**
     * @brief Implements IProcessor for WebP files using libwebp.
     *
     * @details This processor performs a decode and re-encode cycle
     * *only* for lossless WebP files (static or animated), applying
     * the highest compression settings (`-m 6`, `-q 100` equivalent).
     * Lossy WebP files, and animated WebPs with any lossy frame, are skipped.
     */
    class WebpProcessor final : public IProcessor {
    public:
        // --- self-description ---
        [[nodiscard]] std::string_view get_name() const noexcept override {
            return "WebpProcessor";
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_mime_types() const noexcept override {
            static constexpr std::array<std::string_view, 2> kMimes = { "image/webp", "image/x-webp" };
            return {kMimes.data(), kMimes.size()};
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_extensions() const noexcept override {
            static constexpr std::array<std::string_view, 1> kExts = { ".webp" };
            return {kExts.data(), kExts.size()};
        }

        // --- capabilities ---
        [[nodiscard]] bool can_recompress() const noexcept override { return true; }
        [[nodiscard]] bool can_extract_contents() const noexcept override { return false; }

        // --- operations ---

        /**
         * @brief Recompresses a lossless WebP file using libwebp.
         *
         * If the input file is lossy (or an animation with any lossy frame),
         * it is skipped. Static lossless files are decoded and re-encoded
         * at the maximum lossless preset (level 9). Animated lossless files
         * are decoded frame-by-frame via WebPAnimDecoder and re-assembled via
         * WebPAnimEncoder, which picks minimal per-frame regions/dispose/blend.
         *
         * @param input Path to the source WebP file.
         * @param output Path to write the optimized WebP file.
         * @param options Processing options.
         * @throws std::runtime_error if libwebp init or processing fails.
         */
        void recompress(const std::filesystem::path& input,
                        const std::filesystem::path& output, const ProcessingOptions &options) override;

        /**
         * @brief WebP is not a container format.
         * @return std::nullopt
         */
        std::optional<ExtractedContent> prepare_extraction(
            [[maybe_unused]] const std::filesystem::path& input_path) override { return std::nullopt; }

        /**
         * @brief WebP is not a container format.
         * @return Empty path.
         */
        std::filesystem::path finalize_extraction(const ExtractedContent &, const ProcessingOptions &options) override {return {};}

        // --- integrity check ---

        /**
         * @brief Compares two WebP files pixel by pixel.
         *
         * For static images, decodes both into a raw RGBA8 buffer and
         * compares dimensions and pixels. For animations, decodes both
         * via WebPAnimDecoder and compares canvas size, loop count, and
         * every frame's timestamp and fully-composited pixel data.
         *
         * @param a Path to the first WebP file.
         * @param b Path to the second WebP file.
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

#endif // CHISEL_WEBP_PROCESSOR_HPP