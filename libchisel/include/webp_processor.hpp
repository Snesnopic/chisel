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
     * *only* for lossless WebP files, applying the highest
     * compression settings (`-m 6`, `-q 100` equivalent).
     * Lossy WebP files are skipped.
     */
    class WebpProcessor final : public IProcessor {
    public:
        // --- self-description ---
        [[nodiscard]] std::string_view get_name() const noexcept override {
            return "WebpProcessor";
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_mime_types() const noexcept override {
            static constexpr std::array<std::string_view, 1> kMimes = { "image/webp" };
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
         * If the input file is lossy, it is skipped.
         * If lossless, it performs a full decode and re-encode cycle
         * using the maximum lossless preset (level 9).
         *
         * @param input Path to the source WebP file.
         * @param output Path to write the optimized WebP file.
         * @param preserve_metadata If true, copies EXIF, XMP, and ICCP chunks
         * from the original file using the WebPMux API.
         * @throws std::runtime_error if libwebp init or processing fails.
         */
        void recompress(const std::filesystem::path& input,
                        const std::filesystem::path& output,
                        bool preserve_metadata) override;

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
        std::filesystem::path finalize_extraction(const ExtractedContent &,
                                                  [[maybe_unused]] ContainerFormat target_format) override {return {};}

        // --- integrity check ---

        /**
         * @brief (Not Implemented) Compute a raw checksum.
         * @param file_path Path to the file.
         * @return An empty string.
         */
        [[nodiscard]] std::string get_raw_checksum(const std::filesystem::path& file_path) const override;
    };

} // namespace chisel

#endif // CHISEL_WEBP_PROCESSOR_HPP