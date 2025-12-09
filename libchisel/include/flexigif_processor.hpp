//
// Created by Giuseppe Francione on 19/10/25.
//

/**
 * @file flexigif_processor.hpp
 * @brief Defines the IProcessor implementation for GIF files using flexigif.
 */

#ifndef CHISEL_FLEXIGIF_PROCESSOR_HPP
#define CHISEL_FLEXIGIF_PROCESSOR_HPP

#include "processor.hpp"
#include <array>
#include <string_view>
#include <span>

namespace chisel {

    /**
     * @brief Implements IProcessor for GIF files using the flexigif library.
     *
     * @details This processor performs a lossless recompression by decoding
     * the GIF, running LZW optimization passes (partial and final),
     * and then re-encoding the LZW bitstream.
     */
    class FlexiGifProcessor final : public IProcessor {
    public:
        // --- self-description ---
        [[nodiscard]] std::string_view get_name() const noexcept override {
            return "FlexigifProcessor";
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_mime_types() const noexcept override {
            static constexpr std::array<std::string_view, 1> kMimes = { "image/gif" };
            return {kMimes.data(), kMimes.size()};
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_extensions() const noexcept override {
            static constexpr std::array<std::string_view, 1> kExts = { ".gif" };
            return {kExts.data(), kExts.size()};
        }

        // --- capabilities ---
        [[nodiscard]] bool can_recompress() const noexcept override { return true; }
        [[nodiscard]] bool can_extract_contents() const noexcept override { return false; }

        // --- operations ---

        /**
         * @brief Losslessly recompresses a GIF file using flexigif.
         *
         * Decodes the GIF, then uses LzwEncoder::optimizePartial and
         * LzwEncoder::optimize to find a more optimal LZW encoding.
         * The result is written using `writeOptimized`.
         *
         * @param input Path to the source GIF file.
         * @param output Path to write the optimized GIF file.
         * @param preserve_metadata This processor currently does not
         * support metadata preservation.
         * @throws std::runtime_error if flexigif init or processing fails.
         */
        void recompress(const std::filesystem::path& input,
                        const std::filesystem::path& output,
                        bool preserve_metadata) override;

        [[nodiscard]] bool raw_equal(const std::filesystem::path &a, const std::filesystem::path &b) const override;

        /**
         * @brief GIF is not a container format.
         * @return std::nullopt
         */
        std::optional<ExtractedContent> prepare_extraction(
            [[maybe_unused]] const std::filesystem::path& input_path) override { return std::nullopt; }

        /**
         * @brief GIF is not a container format.
         * @return Empty path.
         */
        std::filesystem::path finalize_extraction(const ExtractedContent &) override {return {};}

        // --- integrity check ---

        /**
         * @brief (Not Implemented) Compute a raw checksum.
         * @param file_path Path to the file.
         * @return An empty string.
         * @note This means raw_equal() will always return true.
         */
        [[nodiscard]] std::string get_raw_checksum(const std::filesystem::path& file_path) const override;
    };

} // namespace chisel

#endif // CHISEL_FLEXIGIF_PROCESSOR_HPP