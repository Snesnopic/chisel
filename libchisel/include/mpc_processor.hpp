//
// Created by Giuseppe Francione on 24/03/26.
//

/**
 * @file mpc_processor.hpp
 * @brief Processor for MPC files.
 */

#ifndef CHISEL_MPC_PROCESSOR_HPP
#define CHISEL_MPC_PROCESSOR_HPP

#include "processor.hpp"
#include <array>
#include <string_view>
#include <span>

namespace chisel {

    /**
     * @brief Processor implementation for Mpc files.
     */
    class MpcProcessor final : public IProcessor {
    public:
        [[nodiscard]] std::string_view get_name() const noexcept override {
            return "MpcProcessor";
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_mime_types() const noexcept override {
            static constexpr std::array<std::string_view, 2> kMimes = {
                "audio/musepack", "audio/x-musepack"
            };
            return {kMimes.data(), kMimes.size()};
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_extensions() const noexcept override {
            static constexpr std::array<std::string_view, 3> kExts = { ".mpc", ".mp+", ".mpp" };
            return {kExts.data(), kExts.size()};
        }

        [[nodiscard]] bool can_recompress() const noexcept override { return false; }
        [[nodiscard]] bool can_extract_contents() const noexcept override { return true; }

        void recompress(const std::filesystem::path& input,
                        const std::filesystem::path& output, const ProcessingOptions &options) override;

        std::optional<ExtractedContent> prepare_extraction(
            const std::filesystem::path& input_path) override;

        std::filesystem::path finalize_extraction(const ExtractedContent &content, const ProcessingOptions &options) override;

        [[nodiscard]] std::string get_raw_checksum(const std::filesystem::path& file_path) const override { return ""; }
    };

} // namespace chisel

#endif // CHISEL_MPC_PROCESSOR_HPP