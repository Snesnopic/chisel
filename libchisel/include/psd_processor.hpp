//
// Created by Giuseppe Francione on 24/09/26.
//

/**
 * @file psd_processor.hpp
 * @brief Defines the IProcessor implementation for Photoshop documents.
 */

#ifndef CHISEL_PSD_PROCESSOR_HPP
#define CHISEL_PSD_PROCESSOR_HPP

#include "processor.hpp"
#include <array>

namespace chisel {

/**
 * @brief Implements IProcessor for Photoshop documents (PSD and PSB).
 *
 * @details Recompression re-encodes the RLE (PackBits) channel data of the layers and of the
 * composite image with the shortest encoding, leaving every other byte as it was. As a
 * container, it extracts the files embedded in smart objects and the JPEG thumbnail for the
 * executor to optimize, and puts back the ones that got smaller.
 */
class PsdProcessor final : public IProcessor {
public:
    // --- self-description ---
    [[nodiscard]] std::string_view get_name() const noexcept override {
        return "PsdProcessor";
    }

    [[nodiscard]] std::span<const std::string_view> get_supported_mime_types() const noexcept override {
        static constexpr std::array<std::string_view, 2> kMimes = { "image/vnd.adobe.photoshop", "image/x-photoshop" };
        return {kMimes.data(), kMimes.size()};
    }

    [[nodiscard]] std::span<const std::string_view> get_supported_extensions() const noexcept override {
        static constexpr std::array<std::string_view, 2> kExts = { ".psd", ".psb" };
        return {kExts.data(), kExts.size()};
    }

    // --- capabilities ---
    [[nodiscard]] bool can_recompress() const noexcept override { return true; }
    [[nodiscard]] bool can_extract_contents() const noexcept override { return true; }

    // --- operations ---

    /**
     * @brief Re-encodes the RLE channel data with the shortest PackBits encoding.
     * @param input_path Path to the document.
     * @param output_path Where the result is written (a copy of the input if it can't be parsed).
     * @param options Processing options.
     */
    void recompress(const std::filesystem::path& input_path,
                    const std::filesystem::path& output_path, const ProcessingOptions& options) override;

    /**
     * @brief Extracts the JPEG thumbnail and the files embedded in smart objects.
     * @param input_path Path to the document.
     * @return The extracted files, possibly none.
     */
    std::optional<ExtractedContent> prepare_extraction(const std::filesystem::path& input_path) override;

    /**
     * @brief Puts back the extracted files that got smaller.
     * @param content The ExtractedContent struct from `prepare_extraction`.
     * @param options Processing options.
     * @return Path to the rebuilt document, or an empty path if nothing got smaller.
     */
    std::filesystem::path finalize_extraction(const ExtractedContent& content, const ProcessingOptions& options) override;

    // --- integrity check ---

    /**
     * @brief Not used: raw_equal compares documents structurally.
     * @return An empty string.
     */
    [[nodiscard]] std::string get_raw_checksum(const std::filesystem::path& file_path) const override;

    /**
     * @brief Checks that two documents are identical once their RLE data is decoded.
     * @param a First document.
     * @param b Second document.
     * @return true if every byte other than the RLE encoding and the lengths depending on it matches.
     */
    [[nodiscard]] bool raw_equal(const std::filesystem::path& a, const std::filesystem::path& b) const override;
};

} // namespace chisel

#endif // CHISEL_PSD_PROCESSOR_HPP
