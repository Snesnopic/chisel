//
// Created by Giuseppe Francione on 15/08/26.
//

/**
 * @file optigif_processor.hpp
 * @brief Defines the IProcessor implementation for GIF files using optigif.
 */

#ifndef CHISEL_OPTIGIF_PROCESSOR_HPP
#define CHISEL_OPTIGIF_PROCESSOR_HPP

#include "processor.hpp"
#include <array>
#include <string_view>
#include <span>

namespace chisel {

/**
 * @brief Implements IProcessor for GIF files using optigif.
 *
 * optigif rebuilds frame structure (crop, disposal, transparency, palettes)
 * and searches LZW dictionary restart points on the same in-memory model.
 * Registered in ProcessorRegistry in place of the old gifsicle-based
 * GifProcessor and flexiGIF-based FlexiGifProcessor, which each only did one
 * half of that job and handed the file back and forth; both have since been
 * removed.
 */
class OptigifProcessor final : public IProcessor {
public:
    // --- self-description ---
    [[nodiscard]] std::string_view get_name() const noexcept override {
        return "OptigifProcessor";
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
     * @brief Losslessly recompresses a GIF file using optigif.
     * @param input Path to the source GIF file.
     * @param output Path to write the optimized GIF file.
     * @param options Processing options.
     * @throws std::runtime_error if optigif fails to read or process the file.
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
    std::filesystem::path finalize_extraction(const ExtractedContent &, const ProcessingOptions &options) override { return {}; }

    /**
     * @brief Compares two GIF files by decoding both to rendered frames and delays.
     * @param a First GIF file.
     * @param b Second GIF file.
     * @return true if every decoded frame and delay is identical.
     */
    [[nodiscard]] bool raw_equal(const std::filesystem::path &a, const std::filesystem::path &b) const override;

    // --- integrity check ---

    /**
     * @brief (Not implemented) Compute a raw checksum.
     * @return An empty string.
     * @note Unused: the executor only calls raw_equal() for recompressing
     *       processors, and raw_equal() here does a real pixel/delay
     *       comparison rather than relying on this checksum.
     */
    [[nodiscard]] std::string get_raw_checksum(const std::filesystem::path& file_path) const override;
};

} // namespace chisel

#endif //CHISEL_OPTIGIF_PROCESSOR_HPP
