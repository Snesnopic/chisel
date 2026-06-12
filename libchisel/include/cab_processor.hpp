//
// Created by Giuseppe Francione on 05/06/26.
//

/**
 * @file cab_processor.hpp
 * @brief Defines the IProcessor implementation for Microsoft Cabinet (.cab) files.
 */

#ifndef CHISEL_CAB_PROCESSOR_HPP
#define CHISEL_CAB_PROCESSOR_HPP

#include "processor.hpp"
#include <array>
#include <string_view>
#include <span>

namespace chisel {

/**
 * @brief Implements IProcessor for Microsoft Cabinet (.cab) files.
 *
 * @details Recompresses MSZIP (raw Deflate) blocks within MSCF cabinet files
 * using libdeflate at the maximum level. Multi-volume and signed CABs are
 * skipped. Only folders using MSZIP compression (type 0x0001) are touched;
 * other folder types are copied verbatim.
 */
class CabProcessor final : public IProcessor {
public:
    // --- self-description ---

    [[nodiscard]] std::string_view get_name() const noexcept override {
        return "CabProcessor";
    }

    [[nodiscard]] std::span<const std::string_view> get_supported_mime_types() const noexcept override {
        static constexpr std::array<std::string_view, 2> kMimes = {
            "application/vnd.ms-cab-compressed",
            "application/x-cab"
        };
        return {kMimes.data(), kMimes.size()};
    }

    [[nodiscard]] std::span<const std::string_view> get_supported_extensions() const noexcept override {
        static constexpr std::array<std::string_view, 1> kExts = { ".cab" };
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
                                 const std::filesystem::path& b) const override {
        // CAB is a container: delegate to default byte-level comparison.
        return read_file(a) == read_file(b);
    }
};

} // namespace chisel

#endif // CHISEL_CAB_PROCESSOR_HPP
