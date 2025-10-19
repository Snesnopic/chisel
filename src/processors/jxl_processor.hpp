//
// Created by Giuseppe Francione on 19/10/25.
//

#ifndef CHISEL_JXL_PROCESSOR_HPP
#define CHISEL_JXL_PROCESSOR_HPP

#include "processor.hpp"
#include "../utils/logger.hpp"
#include <array>
#include <string_view>
#include <span>

namespace chisel {

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
        void recompress(const std::filesystem::path& input,
                        const std::filesystem::path& output,
                        bool preserve_metadata) override;

        std::optional<ExtractedContent> prepare_extraction(
            [[maybe_unused]] const std::filesystem::path& input_path) override { return std::nullopt; }

        void finalize_extraction(const ExtractedContent&,
                                 [[maybe_unused]] ContainerFormat target_format) override {}

        // --- integrity check ---
        [[nodiscard]] std::string get_raw_checksum(const std::filesystem::path& file_path) const override;
    };

} // namespace chisel

#endif // CHISEL_JXL_PROCESSOR_HPP