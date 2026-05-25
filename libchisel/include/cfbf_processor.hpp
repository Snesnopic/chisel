//
// Created by Giuseppe Francione on 24/03/26.
//

/**
 * @file cfbf_processor.hpp
 * @brief Processor for CFBF files.
 */

#ifndef CHISEL_CFBF_PROCESSOR_HPP
#define CHISEL_CFBF_PROCESSOR_HPP

#include "processor.hpp"
#include <array>
#include <string_view>
#include <span>

namespace chisel {

    /**
     * @brief Processor for CFBF (Compound File Binary Format) files.
     * @details Optimizes .doc, .xls, .ppt, and .msi files on Windows by removing
     * unused sectors and defragmenting the OLE storage container.
     */
    class CfbfProcessor final : public IProcessor {
    public:
        [[nodiscard]] std::string_view get_name() const noexcept override {
            return "CfbfProcessor";
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_mime_types() const noexcept override {
            static constexpr std::array<std::string_view, 5> kMimes = {
                "application/x-ole-storage",
                "application/msword",
                "application/vnd.ms-excel",
                "application/vnd.ms-powerpoint",
                "application/x-msi"
            };
            return {kMimes.data(), kMimes.size()};
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_extensions() const noexcept override {
            static constexpr std::array<std::string_view, 4> kExts = {
                ".doc", ".xls", ".ppt", ".msi"
            };
            return {kExts.data(), kExts.size()};
        }

        [[nodiscard]] bool can_recompress() const noexcept override {
#ifdef _WIN32
            return true;
#else
            return false;
#endif
        }

        [[nodiscard]] bool can_extract_contents() const noexcept override { return false; }

        void recompress(const std::filesystem::path& input,
                        const std::filesystem::path& output, const ProcessingOptions &options) override;

        std::optional<ExtractedContent> prepare_extraction(const std::filesystem::path& input_path) override;

        std::filesystem::path finalize_extraction(const ExtractedContent &content, const ProcessingOptions &options) override;

        [[nodiscard]] std::string get_raw_checksum(const std::filesystem::path& file_path) const override { return ""; }
    };

} // namespace chisel

#endif // CHISEL_CFBF_PROCESSOR_HPP