//
// Created by Giuseppe Francione on 19/10/25.
//

/**
 * @file wavpack_processor.hpp
 * @brief Defines the IProcessor implementation for WavPack audio files.
 */

#ifndef CHISEL_WAVPACK_PROCESSOR_HPP
#define CHISEL_WAVPACK_PROCESSOR_HPP

#include "processor.hpp"
#include <array>
#include <string_view>
#include <span>

namespace chisel {

    /**
     * @brief Implements IProcessor for WavPack files (.wv) using libwavpack.
     *
     * Performs a full decode and re-encode cycle using the highest
     * compression settings. Also handles copying of APEv2 tags.
     */
    class WavPackProcessor final : public IProcessor {
    public:
        // --- self-description ---
        [[nodiscard]] std::string_view get_name() const noexcept override {
            return "WavpackProcessor";
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_mime_types() const noexcept override {
            static constexpr std::array<std::string_view, 1> kMimes = { "audio/x-wavpack" };
            return {kMimes.data(), kMimes.size()};
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_extensions() const noexcept override {
            static constexpr std::array<std::string_view, 3> kExts = { ".wv", ".wvp", ".wvc" };
            return {kExts.data(), kExts.size()};
        }

        // --- capabilities ---
        [[nodiscard]] bool can_recompress() const noexcept override { return true; }
        [[nodiscard]] bool can_extract_contents() const noexcept override { return false; }

        // --- operations ---

        /**
         * @brief Recompresses a WavPack file using libwavpack.
         *
         * Performs a full decode and re-encode cycle using the
         * "very high" compression flag (CONFIG_VERY_HIGH_FLAG).
         * This processor correctly handles both single (.wv) and
         * dual-file (.wv + .wvc) inputs.
         *
         * @param input Path to the source WavPack file (.wv).
         * @param output Path to write the optimized WavPack file.
         * @param preserve_metadata If true, copies APEv2 tags
         * from the input file to the output file.
         * @throws std::runtime_error if the libwavpack encoder or decoder fails.
         */
        void recompress(const std::filesystem::path& input,
                        const std::filesystem::path& output,
                        bool preserve_metadata) override;

        /**
         * @brief This format cannot be extracted.
         * @return std::nullopt
         */
        std::optional<ExtractedContent> prepare_extraction(
            [[maybe_unused]] const std::filesystem::path& input_path) override { return std::nullopt; }

        /**
         * @brief This format cannot be extracted.
         * @return Empty path.
         */
        std::filesystem::path finalize_extraction(const ExtractedContent &,
                                                  [[maybe_unused]] ContainerFormat target_format) override {return {};}

        // --- integrity check ---

        /**
         * @brief (Not Implemented) Compute a raw checksum.
         * @param file_path Path to the file.
         * @return An empty string.
         */
        [[nodiscard]] std::string get_raw_checksum(const std::filesystem::path& file_path) const override;

        /**
         * @brief Compares two WavPack files by decoding them to raw PCM and comparing.
         *
         * @param a First WavPack file.
         * @param b Second WavPack file.
         * @return true if the decoded PCM data and audio parameters are identical.
         */
        [[nodiscard]] bool raw_equal(const std::filesystem::path &a, const std::filesystem::path &b) const override;
    };

} // namespace chisel

#endif // CHISEL_WAVPACK_PROCESSOR_HPP