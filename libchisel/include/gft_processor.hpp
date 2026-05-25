//
// Created by Giuseppe Francione on 26/03/26.
//

/**
 * @file gft_processor.hpp
 * @brief Processor for GFT files.
 */

#ifndef CHISEL_GFT_PROCESSOR_HPP
#define CHISEL_GFT_PROCESSOR_HPP

#include "processor.hpp"
#include <array>
#include <string_view>
#include <span>
#include "file_type.hpp"

namespace chisel {

    /**
     * @brief Processor implementation for Gft files.
     */
    class GftProcessor final : public IProcessor {
    public:
        [[nodiscard]] std::string_view get_name() const noexcept override {
            return "GftProcessor";
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_mime_types() const noexcept override {
            // GFT doesn't have an official MIME, we match by extension fallback
            static constexpr std::array<std::string_view, 1> kMimes = { "application/x-gft" };
            return {kMimes.data(), kMimes.size()};
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_extensions() const noexcept override {
            static constexpr std::array<std::string_view, 1> kExts = { ".gft" };
            return {kExts.data(), kExts.size()};
        }

        [[nodiscard]] bool can_recompress() const noexcept override { return false; }
        [[nodiscard]] bool can_extract_contents() const noexcept override { return true; }

        void recompress(const std::filesystem::path&,
                        const std::filesystem::path&, const ProcessingOptions&) override {}

        std::optional<ExtractedContent> prepare_extraction(const std::filesystem::path& input_path) override;

        std::filesystem::path finalize_extraction(const ExtractedContent& content, const ProcessingOptions& options) override;

        [[nodiscard]] std::string get_raw_checksum(const std::filesystem::path&) const override { return ""; }

        [[nodiscard]] bool raw_equal(const std::filesystem::path &a, const std::filesystem::path &b) const override;
    };

} // namespace chisel

#endif // CHISEL_GFT_PROCESSOR_HPP
