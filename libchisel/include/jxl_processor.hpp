//
// Created by Giuseppe Francione on 19/10/25.
//

/**
 * @file jxl_processor.hpp
 * @brief Defines the IProcessor implementation for JPEG XL files.
 */

#ifndef CHISEL_JXL_PROCESSOR_HPP
#define CHISEL_JXL_PROCESSOR_HPP

#include "processor.hpp"
#include <array>
#include <string_view>
#include <span>

namespace chisel {

    /**
     * @brief Implements IProcessor for JPEG XL (.jxl) files using libjxl.
     *
     * @details This processor performs a full decode and re-encode cycle
     * to ensure the file is saved with lossless settings
     * (e.g., "modular" mode, effort 9).
     */
    class JxlProcessor final : public IProcessor {
    public:
        // --- self-description ---
        [[nodiscard]] std::string_view get_name() const noexcept override {
            return "JxlProcessor";
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_mime_types() const noexcept override {
            static constexpr std::array<std::string_view, 1> kMimes = { "image/jxl" };
            return {kMimes.data(), kMimes.size()};
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_extensions() const noexcept override {
            static constexpr std::array<std::string_view, 1> kExts = { ".jxl" };
            return {kExts.data(), kExts.size()};
        }

        // --- capabilities ---
        [[nodiscard]] bool can_recompress() const noexcept override { return true; }
        [[nodiscard]] bool can_extract_contents() const noexcept override { return false; }

        // --- operations ---

        /**
         * @brief Recompresses a JXL file losslessly using libjxl.
         *
         * Performs a full decode and re-encode cycle, forcing
         * lossless settings (JxlEncoderSetFrameLossless) and
         * high effort (level 9).
         *
         * @param input Path to the source JXL file.
         * @param output Path to write the optimized JXL file.
         * @param preserve_metadata If true, attempts to store
         * JPEG metadata (if any was present).
         * @throws std::runtime_error if libjxl init or processing fails.
         */
        void recompress(const std::filesystem::path& input,
                        const std::filesystem::path& output,
                        bool preserve_metadata) override;

        /**
         * @brief JXL is not a container format.
         * @return std::nullopt
         */
        std::optional<ExtractedContent> prepare_extraction(
            [[maybe_unused]] const std::filesystem::path& input_path) override { return std::nullopt; }

        /**
         * @brief JXL is not a container format.
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

#endif // CHISEL_JXL_PROCESSOR_HPP