//
// Created by Giuseppe Francione on 19/10/25.
//

/**
 * @file flac_processor.hpp
 * @brief Defines the IProcessor implementation for FLAC audio files.
 */

#ifndef CHISEL_FLAC_PROCESSOR_HPP
#define CHISEL_FLAC_PROCESSOR_HPP

#include "processor.hpp"
#include <FLAC/all.h>
#include <array>
#include <string_view>
#include <span>

namespace chisel {

/**
 * @brief Implements IProcessor for FLAC files using libFLAC.
 *
 * This processor performs a full decode and re-encode cycle to achieve
 * maximum lossless compression. It also supports metadata preservation
 * and raw data integrity checks via STREAMINFO MD5.
 */
class FlacProcessor final : public IProcessor {
public:

    // --- self-description ---
    [[nodiscard]] std::string_view get_name() const noexcept override {
        return "FlacProcessor";
    }

    [[nodiscard]] std::span<const std::string_view> get_supported_mime_types() const noexcept override {
        static constexpr std::array<std::string_view, 2> kMimes = { "audio/flac", "audio/x-flac" };
        return {kMimes.data(), kMimes.size()};
    }

    [[nodiscard]] std::span<const std::string_view> get_supported_extensions() const noexcept override {
        static constexpr std::array<std::string_view, 1> kExts = { ".flac" };
        return {kExts.data(), kExts.size()};
    }

    // --- capabilities ---
    [[nodiscard]] bool can_recompress() const noexcept override { return true; }
    /**
     * @brief This processor can extract cover art.
     * @return true
     */
    [[nodiscard]] bool can_extract_contents() const noexcept override { return true; }

    /**
     * @brief Prepares extraction of embedded cover art.
     * @param input_path Path to the FLAC file.
     * @return An ExtractedContent struct containing cover art files and state.
     */
    std::optional<ExtractedContent> prepare_extraction(const std::filesystem::path& input_path) override;

    /**
     * @brief Rebuilds the FLAC file with optimized cover art.
     * @param content The ExtractedContent struct from prepare_extraction.
     * @param target_format (Ignored)
     * @return Path to the newly finalized FLAC file.
     */
    std::filesystem::path finalize_extraction(const ExtractedContent &content) override;
    /**
     * @brief Recompresses a FLAC file using libFLAC.
     *
     * Performs a full decode and re-encode cycle using the highest
     * compression settings (level 8, exhaustive model search).
     *
     * @param input Path to the source FLAC file.
     * @param output Path to write the optimized FLAC file.
     * @param preserve_metadata If true, copies all metadata blocks
     * (except STREAMINFO) to the new file.
     * @throws std::runtime_error if libFLAC init or processing fails.
     */
    void recompress(const std::filesystem::path &input, const std::filesystem::path &output, bool preserve_metadata) override;

    // --- integrity check ---

    /**
     * @brief Gets the MD5 checksum from the FLAC STREAMINFO block.
     * @param file_path Path to the FLAC file.
     * @return A 32-character hex string of the raw audio MD5.
     * @throws std::runtime_error if the STREAMINFO block cannot be read.
     */
    [[nodiscard]] std::string get_raw_checksum(const std::filesystem::path& file_path) const override;

    /**
     * @brief Compares two FLAC files by decoding them to raw PCM and comparing.
     *
     * This is a fallback comparison used if the STREAMINFO MD5s differ
     * (e.g., if one file lacks it).
     *
     * @param a First FLAC file.
     * @param b Second FLAC file.
     * @return true if the decoded PCM data and audio parameters are identical.
     */
    [[nodiscard]] bool raw_equal(const std::filesystem::path &a, const std::filesystem::path &b) const override;
};

} // namespace chisel

#endif //CHISEL_FLAC_PROCESSOR_HPP