//
// Created by Giuseppe Francione on 19/10/25.
//

#ifndef CHISEL_MKV_PROCESSOR_HPP
#define CHISEL_MKV_PROCESSOR_HPP

#include "processor.hpp"
#include <array>
#include <string_view>
#include <span>

namespace chisel {

    class MkvProcessor final : public IProcessor {
    public:
        // --- self-description ---
        [[nodiscard]] std::string_view get_name() const noexcept override {
            return "MkvProcessor";
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_mime_types() const noexcept override {
            static constexpr std::array<std::string_view, 2> kMimes = { "video/x-matroska", "video/webm" };
            return {kMimes.data(), kMimes.size()};
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_extensions() const noexcept override {
            static constexpr std::array<std::string_view, 2> kExts = { ".mkv", ".webm" };
            return {kExts.data(), kExts.size()};
        }

        // --- capabilities ---
        [[nodiscard]] bool can_recompress() const noexcept override { return true; }
        [[nodiscard]] bool can_extract_contents() const noexcept override { return true; } // in futuro

        // --- operations ---

        /**
         * @brief Recompresses a Matroska file.
         * @param input Path to the source MKV/WebM file.
         * @param output Path to write the optimized file.
         * @param options Processing options.
         */
        void recompress(const std::filesystem::path& input,
                        const std::filesystem::path& output, const ProcessingOptions &options) override;

        /**
         * @brief Prepares extraction of MKV contents.
         * @param input_path Path to the MKV file.
         * @return ExtractedContent struct or nullopt.
         */
        std::optional<ExtractedContent> prepare_extraction(
            const std::filesystem::path& input_path) override;

        /**
         * @brief Rebuilds the MKV file from extracted contents.
         * @param content The ExtractedContent struct.
         * @param options Processing options.
         * @return Path to the finalized MKV file.
         */
        std::filesystem::path finalize_extraction(const ExtractedContent &content, const ProcessingOptions &options) override;

        // --- integrity check ---

        /**
         * @brief Compute a raw checksum.
         * @param file_path Path to the file.
         * @return Checksum string.
         */
        [[nodiscard]] std::string get_raw_checksum(const std::filesystem::path& file_path) const override;
    };

} // namespace chisel

#endif // CHISEL_MKV_PROCESSOR_HPP