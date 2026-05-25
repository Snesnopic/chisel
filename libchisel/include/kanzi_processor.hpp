//
// Created by Giuseppe Francione on 20/05/2026.
//

/**
 * @file kanzi_processor.hpp
 * @brief Processor for KANZI files.
 */

#ifndef CHISEL_KANZI_PROCESSOR_HPP
#define CHISEL_KANZI_PROCESSOR_HPP

#include "processor.hpp"
#include <array>
#include <string_view>
#include <span>

namespace chisel {

    /**
     * @brief Processor implementation for Kanzi files.
     */
    class KanziProcessor : public IProcessor {
    public:
        [[nodiscard]] std::string_view get_name() const noexcept override {
            return "KanziProcessor";
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_mime_types() const noexcept override {
            static constexpr std::array<std::string_view, 1> mimes = {
                "application/x-kanzi"
            };
            return mimes;
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_extensions() const noexcept override {
            static constexpr std::array<std::string_view, 1> exts = {
                ".knz"
            };
            return exts;
        }

        [[nodiscard]] bool can_recompress() const noexcept override {
            return false; // stream container, actual data is recompressed by pipeline
        }

        [[nodiscard]] bool can_extract_contents() const noexcept override {
            return true;
        }

        void recompress(const std::filesystem::path& /*input_path*/, const std::filesystem::path& /*output_path*/, const ProcessingOptions& /*options*/) override {
            throw std::logic_error("KanziProcessor does not support direct recompression.");
        }

        std::optional<ExtractedContent> prepare_extraction(const std::filesystem::path& input_path) override;

        std::filesystem::path finalize_extraction(const ExtractedContent& content,
                                                  const ProcessingOptions& options) override;

        [[nodiscard]] std::string get_raw_checksum(const std::filesystem::path& file_path) const override;
    };

} // namespace chisel

#endif // CHISEL_KANZI_PROCESSOR_HPP
