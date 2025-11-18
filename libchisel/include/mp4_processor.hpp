//
// Created by Giuseppe Francione on 18/11/25.
//

#ifndef CHISEL_MP4_PROCESSOR_HPP
#define CHISEL_MP4_PROCESSOR_HPP

#include "processor.hpp"
#include <array>
#include <string_view>
#include <span>

namespace chisel {

    /**
     * @brief Implements IProcessor for MP4/M4A files.
     *
     * @details Extracts and re-inserts cover art (atom 'covr') using
     * AudioMetadataUtil. Stream recompression is not yet implemented.
     */
    class Mp4Processor final : public IProcessor {
    public:
        [[nodiscard]] std::string_view get_name() const noexcept override {
            return "Mp4Processor";
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_mime_types() const noexcept override {
            static constexpr std::array<std::string_view, 3> kMimes = {
                "audio/mp4", "audio/x-m4a", "video/mp4"
            };
            return {kMimes.data(), kMimes.size()};
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_extensions() const noexcept override {
            static constexpr std::array<std::string_view, 3> kExts = { ".mp4", ".m4a", ".m4b" };
            return {kExts.data(), kExts.size()};
        }

        [[nodiscard]] bool can_recompress() const noexcept override { return false; }
        [[nodiscard]] bool can_extract_contents() const noexcept override { return true; }

        void recompress(const std::filesystem::path& input,
                        const std::filesystem::path& output,
                        bool preserve_metadata) override;

        std::optional<ExtractedContent> prepare_extraction(
            const std::filesystem::path& input_path) override;

        std::filesystem::path finalize_extraction(const ExtractedContent &content,
                                                  ContainerFormat target_format) override;

        [[nodiscard]] std::string get_raw_checksum(const std::filesystem::path& file_path) const override { return ""; }
    };

} // namespace chisel

#endif // CHISEL_MP4_PROCESSOR_HPP