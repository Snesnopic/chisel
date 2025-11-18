//
// Created by Giuseppe Francione on 18/11/25.
//

#ifndef CHISEL_OGG_PROCESSOR_HPP
#define CHISEL_OGG_PROCESSOR_HPP

#include "processor.hpp"
#include <array>
#include <string_view>
#include <span>

namespace chisel {

    /**
     * @brief Implements IProcessor for Ogg (Vorbis/Opus) files.
     *
     * @details Extracts and re-inserts cover art (METADATA_BLOCK_PICTURE) using
     * AudioMetadataUtil. Supports both .ogg (Vorbis) and .opus files.
     */
    class OggProcessor final : public IProcessor {
    public:
        [[nodiscard]] std::string_view get_name() const noexcept override {
            return "OggProcessor";
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_mime_types() const noexcept override {
            static constexpr std::array<std::string_view, 3> kMimes = {
                "audio/ogg", "audio/vorbis", "audio/opus"
            };
            return {kMimes.data(), kMimes.size()};
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_extensions() const noexcept override {
            static constexpr std::array<std::string_view, 3> kExts = { ".ogg", ".opus", ".oga" };
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

#endif // CHISEL_OGG_PROCESSOR_HPP