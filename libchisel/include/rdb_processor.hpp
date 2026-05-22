//
// Created by Giuseppe Francione on 26/03/26.
//

/**
 * @file rdb_processor.hpp
 * @brief Processor for RDB files.
 */

#ifndef CHISEL_RDB_PROCESSOR_HPP
#define CHISEL_RDB_PROCESSOR_HPP

#include "file_type.hpp"
#include "processor.hpp"
#include <array>
#include <string_view>
#include <span>

namespace chisel {

    /**
     * @brief Processor implementation for Rdb files.
     */
    class RdbProcessor final : public IProcessor {
    public:
        [[nodiscard]] std::string_view get_name() const noexcept override {
            return "RdbProcessor";
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_mime_types() const noexcept override {
            static constexpr std::array<std::string_view, 1> kMimes = { "application/x-rdb" };
            return {kMimes.data(), kMimes.size()};
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_extensions() const noexcept override {
            static constexpr std::array<std::string_view, 1> kExts = { ".rdb" };
            return {kExts.data(), kExts.size()};
        }

        [[nodiscard]] bool can_recompress() const noexcept override { return false; }
        [[nodiscard]] bool can_extract_contents() const noexcept override { return true; }

        void recompress(const std::filesystem::path&,
                        const std::filesystem::path&, const ProcessingOptions&) override {}

        std::optional<ExtractedContent> prepare_extraction(const std::filesystem::path& input_path) override;

        std::filesystem::path finalize_extraction(const ExtractedContent& content, const ProcessingOptions& options) override;

        [[nodiscard]] std::string get_raw_checksum(const std::filesystem::path&) const override { return ""; }

        [[nodiscard]] bool raw_equal(const std::filesystem::path &a, const std::filesystem::path &b) const override;
    };

} // namespace chisel

#endif // CHISEL_RDB_PROCESSOR_HPP
