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
 * parameter space than libFLAC's own encoder (used by FlacProcessor), at the
 * cost of a from-scratch decode/encode pass. Cover art is intentionally left
 * to FlacProcessor (can_extract_contents() is false here) so the two never
 * fight over the same PICTURE metadata block.
 *
 * @note Not yet registered in ProcessorRegistry: FlacProcessor and this class
 * are alternative full re-encoders of the same source, not a chain where one
 * improves on the other's output, so PIPE-mode registration would just waste
 * one of the two encodes rather than pick the smaller result. See the
 * registration TODO in processor_registry.cpp for the resolution plan.
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
    // cover art stays FlacProcessor's job; see class doc comment
    [[nodiscard]] bool can_extract_contents() const noexcept override { return false; }

    std::optional<ExtractedContent> prepare_extraction(
        [[maybe_unused]] const std::filesystem::path& input_path) override { return std::nullopt; }

    std::filesystem::path finalize_extraction(const ExtractedContent &, const ProcessingOptions &options) override { return {}; }

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
