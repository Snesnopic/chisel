//
// Created by Giuseppe Francione on 04/06/26.
//

/**
 * @file mng_processor.hpp
 * @brief Defines the IProcessor implementation for MNG and JNG image files.
 */

#ifndef CHISEL_MNG_PROCESSOR_HPP
#define CHISEL_MNG_PROCESSOR_HPP

#include "processor.hpp"
#include <array>
#include <string_view>
#include <span>

namespace chisel {

    /**
     * @brief Implements IProcessor for MNG and JNG files.
     *
     * @details Performs lossless re-compression of MNG (Deflate streams)
     * and JNG (JPEG streams + Deflate alpha channel).
     * Uses Zopfli for Deflate and mozjpeg for JPEG optimization.
     */
    class MngProcessor final : public IProcessor {
    public:
        // --- self-description ---
        [[nodiscard]] std::string_view get_name() const noexcept override {
            return "MngProcessor";
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_mime_types() const noexcept override {
            static constexpr std::array<std::string_view, 2> kMimes = {
                "video/x-mng", "image/x-jng"
            };
            return {kMimes.data(), kMimes.size()};
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_extensions() const noexcept override {
            static constexpr std::array<std::string_view, 2> kExts = { ".mng", ".jng" };
            return {kExts.data(), kExts.size()};
        }

        // --- capabilities ---
        [[nodiscard]] bool can_recompress() const noexcept override { return true; }
        [[nodiscard]] bool can_extract_contents() const noexcept override { return false; }

        // --- operations ---

        /**
         * @brief Recompresses an MNG or JNG file losslessly.
         *
         * @param input Path to the source file.
         * @param output Path to write the optimized file.
         * @param options Processing options (including Zopfli iterations).
         */
        void recompress(const std::filesystem::path& input,
                        const std::filesystem::path& output, const ProcessingOptions &options) override;

        std::optional<ExtractedContent> prepare_extraction(
            [[maybe_unused]] const std::filesystem::path& input_path) override { return std::nullopt; }

        std::filesystem::path finalize_extraction(const ExtractedContent &, const ProcessingOptions &options) override {return {};}

        // --- integrity check ---
        [[nodiscard]] std::string get_raw_checksum(const std::filesystem::path& file_path) const override;

        [[nodiscard]] bool raw_equal(const std::filesystem::path& a,
                                     const std::filesystem::path& b) const override;
    };

} // namespace chisel

#endif // CHISEL_MNG_PROCESSOR_HPP
