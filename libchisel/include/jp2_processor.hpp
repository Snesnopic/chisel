//
// Created by Giuseppe Francione on 04/06/26.
//

/**
 * @file jp2_processor.hpp
 * @brief defines the IProcessor implementation for JPEG 2000 files.
 */

#ifndef CHISEL_JP2_PROCESSOR_HPP
#define CHISEL_JP2_PROCESSOR_HPP

#include "processor.hpp"
#include <array>
#include <string_view>
#include <span>

namespace chisel {

/**
 * @brief implements IProcessor for JPEG 2000 files using libopenjp2.
 *
 * @details performs lossless re-encoding of JPEG 2000 files.
 */
class Jp2Processor : public IProcessor {
public:
    // --- self-description ---

    [[nodiscard]] std::string_view get_name() const noexcept override {
        return "Jp2Processor";
    }

    [[nodiscard]] std::span<const std::string_view> get_supported_mime_types() const noexcept override {
        static constexpr std::array<std::string_view, 2> mimes = {
            "image/jp2",
            "image/jpx"
        };
        return {mimes.data(), mimes.size()};
    }

    [[nodiscard]] std::span<const std::string_view> get_supported_extensions() const noexcept override {
        static constexpr std::array<std::string_view, 3> exts = {
            ".jp2", ".j2k", ".j2c"
        };
        return {exts.data(), exts.size()};
    }

    // --- capabilities ---

    [[nodiscard]] bool can_recompress() const noexcept override { return true; }
    [[nodiscard]] bool can_extract_contents() const noexcept override { return false; }

    // --- operations ---

    void recompress(const std::filesystem::path& input_path,
                    const std::filesystem::path& output_path, const ProcessingOptions &options) override;

    std::optional<ExtractedContent> prepare_extraction(const std::filesystem::path& input_path) override;

    std::filesystem::path finalize_extraction(const ExtractedContent& content, const ProcessingOptions &options) override;

    // --- integrity check ---

    [[nodiscard]] std::string get_raw_checksum(const std::filesystem::path& file_path) const override;

    [[nodiscard]] bool raw_equal(const std::filesystem::path& a,
                                 const std::filesystem::path& b) const override;
};

} // namespace chisel

#endif // CHISEL_JP2_PROCESSOR_HPP
