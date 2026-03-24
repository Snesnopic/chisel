//
// Created by Giuseppe Francione on 23/03/26.
//

#ifndef CHISEL_WOFF2_PROCESSOR_HPP
#define CHISEL_WOFF2_PROCESSOR_HPP

#include "processor.hpp"
#include <array>
#include <string_view>
#include <span>

namespace chisel {

    class Woff2Processor final : public IProcessor {
    public:
        [[nodiscard]] std::string_view get_name() const noexcept override {
            return "Woff2Processor";
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_mime_types() const noexcept override {
            static constexpr std::array<std::string_view, 1> kMimes = { "font/woff2" };
            return {kMimes.data(), kMimes.size()};
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_extensions() const noexcept override {
            static constexpr std::array<std::string_view, 1> kExts = { ".woff2" };
            return {kExts.data(), kExts.size()};
        }

        [[nodiscard]] bool can_recompress() const noexcept override { return true; }
        [[nodiscard]] bool can_extract_contents() const noexcept override { return false; }

        void recompress(const std::filesystem::path& input,
                        const std::filesystem::path& output, const ProcessingOptions &options) override;

        std::optional<ExtractedContent> prepare_extraction(const std::filesystem::path&) override {
            return std::nullopt;
        }

        std::filesystem::path finalize_extraction(const ExtractedContent&, const ProcessingOptions&) override {
            return {};
        }

        [[nodiscard]] std::string get_raw_checksum(const std::filesystem::path& file_path) const override;

        [[nodiscard]] bool raw_equal(const std::filesystem::path &a, const std::filesystem::path &b) const override;
    };

} // namespace chisel

#endif // CHISEL_WOFF2_PROCESSOR_HPP