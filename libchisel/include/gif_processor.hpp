//
// Created by Giuseppe Francione on 19/10/25.
//

#ifndef CHISEL_GIF_PROCESSOR_HPP
#define CHISEL_GIF_PROCESSOR_HPP

#include "processor.hpp"
#include <array>
#include <string_view>
#include <span>

namespace chisel {

    class GifProcessor final : public IProcessor {
    public:
        // --- self-description ---
        [[nodiscard]] std::string_view get_name() const noexcept override {
            return "GifProcessor";
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_mime_types() const noexcept override {
            static constexpr std::array<std::string_view, 1> kMimes = { "image/gif" };
            return {kMimes.data(), kMimes.size()};
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_extensions() const noexcept override {
            static constexpr std::array<std::string_view, 1> kExts = { ".gif" };
            return {kExts.data(), kExts.size()};
        }

        // --- capabilities ---
        [[nodiscard]] bool can_recompress() const noexcept override { return true; }
        [[nodiscard]] bool can_extract_contents() const noexcept override { return false; }

        // --- operations ---

        /**
         * @brief Recompresses a GIF file.
         * @param input Path to the source GIF file.
         * @param output Path to write the optimized GIF file.
         * @param options Processing options.
         */
        void recompress(const std::filesystem::path& input,
                        const std::filesystem::path& output, const ProcessingOptions &options) override;

        /**
         * @brief GIF is not a container format.
         * @return std::nullopt
         */
        std::optional<ExtractedContent> prepare_extraction(
            [[maybe_unused]] const std::filesystem::path& input_path) override { return std::nullopt; }

        /**
         * @brief GIF is not a container format.
         * @return Empty path.
         */
        std::filesystem::path finalize_extraction(const ExtractedContent &, const ProcessingOptions &options) override {return {};}

        /**
         * @brief Compares two GIF files.
         * @param a First GIF file.
         * @param b Second GIF file.
         * @return true if files are identical.
         */
        [[nodiscard]] bool raw_equal(const std::filesystem::path &a, const std::filesystem::path &b) const override;

        // --- integrity check ---

        /**
         * @brief Compute a raw checksum.
         * @param file_path Path to the file.
         * @return Checksum string.
         */
        [[nodiscard]] std::string get_raw_checksum(const std::filesystem::path& file_path) const override;
    };

} // namespace chisel

#endif // CHISEL_GIF_PROCESSOR_HPP