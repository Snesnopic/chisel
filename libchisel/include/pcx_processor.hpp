//
// Created by Giuseppe Francione on 05/06/26.
//

/**
 * @file pcx_processor.hpp
 * @brief Defines the IProcessor implementation for PCX and DCX image files.
 */

#ifndef CHISEL_PCX_PROCESSOR_HPP
#define CHISEL_PCX_PROCESSOR_HPP

#include "processor.hpp"
#include <array>
#include <string_view>
#include <span>

namespace chisel {

/**
 * @brief Implements IProcessor for PCX and DCX files.
 *
 * @details Performs lossless re-compression of PCX using a native RLE encoder.
 * Supports DCX multi-page containers by optimizing each internal PCX page.
 */
class PcxProcessor final : public IProcessor {
public:
    // --- self-description ---
    [[nodiscard]] std::string_view get_name() const noexcept override {
        return "PcxProcessor";
    }

    [[nodiscard]] std::span<const std::string_view> get_supported_mime_types() const noexcept override {
        static constexpr std::array<std::string_view, 2> kMimes = {
            "image/x-pcx", "image/vnd.zbrush.pcx"
        };
        return {kMimes.data(), kMimes.size()};
    }

    [[nodiscard]] std::span<const std::string_view> get_supported_extensions() const noexcept override {
        static constexpr std::array<std::string_view, 3> kExts = { ".pcx", ".dcx", ".pcc" };
        return {kExts.data(), kExts.size()};
    }

    // --- capabilities ---
    [[nodiscard]] bool can_recompress() const noexcept override { return true; }
    [[nodiscard]] bool can_extract_contents() const noexcept override { return false; }

    // --- operations ---
    void recompress(const std::filesystem::path& input,
                    const std::filesystem::path& output, const ProcessingOptions &options) override;

    std::optional<ExtractedContent> prepare_extraction(const std::filesystem::path& input_path) override { return std::nullopt; }

    std::filesystem::path finalize_extraction(const ExtractedContent&, const ProcessingOptions &) override { return {}; }

    // --- integrity check ---
    [[nodiscard]] std::string get_raw_checksum(const std::filesystem::path& file_path) const override;

    [[nodiscard]] bool raw_equal(const std::filesystem::path& a,
                                 const std::filesystem::path& b) const override;
};

} // namespace chisel

#endif // CHISEL_PCX_PROCESSOR_HPP
