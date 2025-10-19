//
// Created by Giuseppe Francione on 19/10/25.
//

#ifndef CHISEL_OOXML_PROCESSOR_HPP
#define CHISEL_OOXML_PROCESSOR_HPP

#include "processor.hpp"
#include "../utils/logger.hpp"
#include <array>
#include <string_view>
#include <span>

namespace chisel {

class OOXMLProcessor final : public IProcessor {
public:
    // --- self-description ---
    [[nodiscard]] std::string_view get_name() const noexcept override {
        return "OOXMLProcessor";
    }

    [[nodiscard]] std::span<const std::string_view> get_supported_mime_types() const noexcept override {
        static constexpr std::array<std::string_view, 3> kMimes = {
            "application/vnd.openxmlformats-officedocument.wordprocessingml.document",
            "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
            "application/vnd.openxmlformats-officedocument.presentationml.presentation"
        };
        return {kMimes.data(), kMimes.size()};
    }

    [[nodiscard]] std::span<const std::string_view> get_supported_extensions() const noexcept override {
        static constexpr std::array<std::string_view, 3> kExts = { ".docx", ".xlsx", ".pptx" };
        return {kExts.data(), kExts.size()};
    }

    // --- capabilities ---
    [[nodiscard]] bool can_recompress() const noexcept override { return false; }
    [[nodiscard]] bool can_extract_contents() const noexcept override { return true; }

    // --- operations ---
    void recompress(const std::filesystem::path&,
                    const std::filesystem::path&,
                    bool) override {}

    std::optional<ExtractedContent> prepare_extraction(
        const std::filesystem::path& input_path) override;

    void finalize_extraction(const ExtractedContent& content,
                             ContainerFormat target_format) override;

    // --- integrity check ---
    [[nodiscard]] std::string get_raw_checksum(const std::filesystem::path& file_path) const override;
};

} // namespace chisel

#endif // CHISEL_OOXML_PROCESSOR_HPP