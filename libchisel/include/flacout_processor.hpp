//
// Created by Giuseppe Francione on 10/08/26.
//

/**
 * @file flacout_processor.hpp
 * @brief Defines the IProcessor implementation for FLAC audio files using flacoutcpp.
 */

#ifndef CHISEL_FLACOUT_PROCESSOR_HPP
#define CHISEL_FLACOUT_PROCESSOR_HPP

#include "processor.hpp"
#include <array>
#include <string_view>
#include <span>

namespace chisel {

/**
 * @brief Implements IProcessor for FLAC files using flacoutcpp.
 *
 * flacoutcpp searches a much larger part of the LPC/apodization/block-size
 * parameter space than libFLAC's own encoder, at the cost of a from-scratch
 * decode/encode pass. Registered in ProcessorRegistry in place of the old
 * libFLAC-based FlacProcessor, which is no longer instantiated.
 *
 * Cover art extraction/reinsertion uses the same shared AudioMetadataUtil
 * logic every other cover-art-only container uses; FlacProcessor used to
 * handle this and it was left unwired when it was retired.
 */
class FlacoutProcessor final : public IProcessor {
public:
    // --- self-description ---
    [[nodiscard]] std::string_view get_name() const noexcept override {
        return "FlacoutProcessor";
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
     * @brief This processor also extracts cover art using AudioMetadataUtil.
     * @return true
     */
    [[nodiscard]] bool can_extract_contents() const noexcept override { return true; }

    /**
     * @brief Extracts embedded cover art via AudioMetadataUtil.
     * @param input_path Path to the source file.
     * @return ExtractedContent with cover art files, or std::nullopt.
     */
    std::optional<ExtractedContent> prepare_extraction(
        const std::filesystem::path& input_path) override;

    /**
     * @brief Re-inserts optimized cover art via AudioMetadataUtil.
     * @param content Content descriptor from prepare_extraction.
     * @param options Processing options.
     * @return Path to the finalized file.
     */
    std::filesystem::path finalize_extraction(const ExtractedContent &content, const ProcessingOptions &options) override;

    /**
     * @brief Recompresses a FLAC file using flacoutcpp's DP-based optimizer.
     * @param input Path to the source FLAC file.
     * @param output Path to write the optimized FLAC file.
     * @param options Processing options.
     * @throws std::runtime_error if flacoutcpp fails to decode or encode the file.
     */
    void recompress(const std::filesystem::path &input, const std::filesystem::path &output, const ProcessingOptions &options) override;

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
     * @param a First FLAC file.
     * @param b Second FLAC file.
     * @return true if the decoded PCM data and audio parameters are identical.
     */
    [[nodiscard]] bool raw_equal(const std::filesystem::path &a, const std::filesystem::path &b) const override;
};

} // namespace chisel

#endif //CHISEL_FLACOUT_PROCESSOR_HPP
