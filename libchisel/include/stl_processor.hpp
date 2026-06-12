//
// Created by Giuseppe Francione on 11/06/26.
//

/**
 * @file stl_processor.hpp
 * @brief Defines the IProcessor implementation for STL 3D model files.
 */

#ifndef CHISEL_STL_PROCESSOR_HPP
#define CHISEL_STL_PROCESSOR_HPP

#include "processor.hpp"
#include <array>
#include <string_view>
#include <span>

namespace chisel {

/**
 * @brief Implements IProcessor for STL 3D model files.
 *
 * @details Converts ASCII STL files to the compact binary format, which is
 * geometrically lossless (coordinates preserved to IEEE 754 float32 precision).
 * Binary STL files are passed through unchanged; if @p preserve_metadata is
 * false, the 80-byte free-form header is zeroed to remove authoring tool info.
 */
class StlProcessor final : public IProcessor {
public:
    // --- self-description ---

    [[nodiscard]] std::string_view get_name() const noexcept override {
        return "StlProcessor";
    }

    [[nodiscard]] std::span<const std::string_view> get_supported_mime_types() const noexcept override {
        static constexpr std::array<std::string_view, 2> kMimes = {
            "model/stl", "model/x.stl-ascii"
        };
        return {kMimes.data(), kMimes.size()};
    }

    [[nodiscard]] std::span<const std::string_view> get_supported_extensions() const noexcept override {
        static constexpr std::array<std::string_view, 1> kExts = { ".stl" };
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

#endif // CHISEL_STL_PROCESSOR_HPP
