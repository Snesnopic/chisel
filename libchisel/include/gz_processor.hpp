//
// Created by Giuseppe Francione on 09/06/26.
//

/**
 * @file gz_processor.hpp
 * @brief Defines the IProcessor implementation for gzip (.gz, .tgz) files.
 */

#ifndef CHISEL_GZ_PROCESSOR_HPP
#define CHISEL_GZ_PROCESSOR_HPP

#include "processor.hpp"
#include <array>
#include <string_view>
#include <span>

namespace chisel {

/**
 * @brief Implements IProcessor for gzip-compressed files (.gz, .tgz, .svgz, etc.).
 *
 * @details Recompresses the raw DEFLATE payload using libdeflate at level 12.
 * When @p preserve_metadata is false, optional gzip header fields (FNAME,
 * FEXTRA, FCOMMENT, FHCRC) are stripped and MTIME is zeroed, matching
 * the behaviour of Leanify. Multi-member gzip files are handled correctly.
 */
class GzProcessor final : public IProcessor {
public:
    // --- self-description ---

    [[nodiscard]] std::string_view get_name() const noexcept override {
        return "GzProcessor";
    }

    [[nodiscard]] std::span<const std::string_view> get_supported_mime_types() const noexcept override {
        static constexpr std::array<std::string_view, 2> kMimes = {
            "application/gzip",
            "application/x-gzip"
        };
        return {kMimes.data(), kMimes.size()};
    }

    [[nodiscard]] std::span<const std::string_view> get_supported_extensions() const noexcept override {
        static constexpr std::array<std::string_view, 5> kExts = {
            ".gz", ".tgz", ".svgz", ".emz", ".wmz"
        };
        return {kExts.data(), kExts.size()};
    }

    // --- capabilities ---

    [[nodiscard]] bool can_recompress() const noexcept override { return true; }
    [[nodiscard]] bool can_extract_contents() const noexcept override { return false; }

    // --- operations ---

    void recompress(const std::filesystem::path& input_path,
                    const std::filesystem::path& output_path,
                    const ProcessingOptions& options) override;

    std::optional<ExtractedContent> prepare_extraction(const std::filesystem::path&) override {
        return std::nullopt;
    }

    std::filesystem::path finalize_extraction(const ExtractedContent&, const ProcessingOptions&) override {
        return {};
    }

    // --- integrity check ---

    [[nodiscard]] std::string get_raw_checksum(const std::filesystem::path& /*file_path*/) const override {
        return "";
    }

    [[nodiscard]] bool raw_equal(const std::filesystem::path& a,
                                 const std::filesystem::path& b) const override;
};

} // namespace chisel

#endif // CHISEL_GZ_PROCESSOR_HPP
