//
// Created by Giuseppe Francione on 26/03/26.
//

#ifndef CHISEL_WOFF_PROCESSOR_HPP
#define CHISEL_WOFF_PROCESSOR_HPP

#include "processor.hpp"
#include <array>
#include <string_view>
#include <span>

namespace chisel {

    class WoffProcessor final : public IProcessor {
    public:
        [[nodiscard]] std::string_view get_name() const noexcept override {
            return "WoffProcessor";
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_mime_types() const noexcept override {
            static constexpr std::array<std::string_view, 2> kMimes = {
                "font/woff",
                "application/font-woff"
            };
            return {kMimes.data(), kMimes.size()};
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_extensions() const noexcept override {
            static constexpr std::array<std::string_view, 1> kExts = { ".woff" };
            return {kExts.data(), kExts.size()};
        }

        [[nodiscard]] bool can_recompress() const noexcept override { return true; }
        [[nodiscard]] bool can_extract_contents() const noexcept override { return false; }

        void recompress(const std::filesystem::path& input_path,
                        const std::filesystem::path& output_path, const ProcessingOptions &options) override;

        std::optional<ExtractedContent> prepare_extraction(const std::filesystem::path&) override { return std::nullopt; }

        std::filesystem::path finalize_extraction(const ExtractedContent&, const ProcessingOptions&) override { return {}; }

        [[nodiscard]] std::string get_raw_checksum(const std::filesystem::path&) const override { return ""; }

        [[nodiscard]] bool raw_equal(const std::filesystem::path &a, const std::filesystem::path &b) const override;
    };

} // namespace chisel

#endif // CHISEL_WOFF_PROCESSOR_HPP
