//
// Created by Giuseppe Francione on 19/10/25.
//

#ifndef CHISEL_ARCHIVE_PROCESSOR_HPP
#define CHISEL_ARCHIVE_PROCESSOR_HPP

#include "processor.hpp"
#include "logger.hpp"
#include <array>
#include <string_view>
#include <span>

namespace chisel {

class ArchiveProcessor final : public IProcessor {
public:
    // --- self-description ---
    [[nodiscard]] std::string_view get_name() const noexcept override {
        return "ArchiveProcessor";
    }

    [[nodiscard]] std::span<const std::string_view> get_supported_mime_types() const noexcept override {
        static constexpr std::array<std::string_view, 6> kMimes = {
            "application/zip",
            "application/x-7z-compressed",
            "application/x-tar",
            "application/vnd.rar",
            "application/x-rar-compressed",
            "application/vnd.comicbook+rar"
        };
        return {kMimes.data(), kMimes.size()};
    }

    [[nodiscard]] std::span<const std::string_view> get_supported_extensions() const noexcept override {
        static constexpr std::array<std::string_view, 6> kExts = {
            ".zip", ".7z", ".tar", ".rar", ".cbz", ".cbr"
        };
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

#endif // CHISEL_ARCHIVE_PROCESSOR_HPP