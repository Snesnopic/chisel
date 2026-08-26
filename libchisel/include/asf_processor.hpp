//
// Created by Giuseppe Francione on 25/08/26.
//

/**
 * @file asf_processor.hpp
 * @brief Defines the IProcessor implementation for the ASF family (WMA/WMV).
 */

#ifndef CHISEL_ASF_PROCESSOR_HPP
#define CHISEL_ASF_PROCESSOR_HPP

#include "processor.hpp"
#include <array>
#include <string_view>
#include <span>

namespace chisel {

/**
 * @brief Implements IProcessor for the ASF family (WMA/WMV).
 *
 * recompress() never touches the Data Object (the actual audio/video
 * payload) -- only the Header Object around it: dropping Padding Objects,
 * and (when options.preserve_metadata is false) Content Description /
 * Extended Content Description objects, then patching the Header Object's
 * and Header Extension Object's own size (and object count) fields to
 * account for the removed bytes.
 *
 * Unlike MP4/QuickTime, ASF's seek indices (Simple Index Object: packet
 * numbers; Index Object: offsets relative to the start of the Data Object)
 * are never expressed as absolute file offsets, so removing bytes from the
 * Header Object never requires patching anything outside it -- confirmed
 * against the ASF specification, not assumed by analogy with MP4.
 *
 * Cover art extraction/reinsertion uses the same shared AudioMetadataUtil
 * logic TagProcessor uses for every other cover-art-only container; this
 * processor replaces TagProcessor for the ASF family specifically, for the
 * same reason Mp4Processor replaces it for MP4: ProcessorExecutor only ever
 * consults the first registered processor for a given format for both
 * capabilities, not a chain of complementary ones.
 */
class AsfProcessor final : public IProcessor {
public:
    // --- self-description ---
    [[nodiscard]] std::string_view get_name() const noexcept override {
        return "AsfProcessor";
    }

    [[nodiscard]] std::span<const std::string_view> get_supported_mime_types() const noexcept override {
        static constexpr std::array<std::string_view, 3> kMimes = {
            "audio/x-ms-wma", "video/x-ms-wmv", "video/x-ms-asf"
        };
        return {kMimes.data(), kMimes.size()};
    }

    [[nodiscard]] std::span<const std::string_view> get_supported_extensions() const noexcept override {
        static constexpr std::array<std::string_view, 3> kExts = { ".wma", ".wmv", ".asf" };
        return {kExts.data(), kExts.size()};
    }

    // --- capabilities ---
    [[nodiscard]] bool can_recompress() const noexcept override { return true; }

    /**
     * @brief This processor also extracts cover art using AudioMetadataUtil.
     * @return true
     */
    [[nodiscard]] bool can_extract_contents() const noexcept override { return true; }

    // --- operations ---

    /**
     * @brief Strips Padding Objects (and, unless preserving metadata,
     * Content Description / Extended Content Description objects) from the
     * Header Object of a WMA/WMV/ASF file, patching container sizes to match.
     * @param input Path to the source file.
     * @param output Path to write the cleaned file.
     * @param options Processing options.
     * @throws std::runtime_error on a malformed or unreadable object structure.
     */
    void recompress(const std::filesystem::path& input,
                    const std::filesystem::path& output, const ProcessingOptions &options) override;

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

    // --- integrity check ---

    /**
     * @brief (Not implemented) Compute a raw checksum.
     * @return An empty string.
     * @note raw_equal() below does a real structural comparison instead.
     */
    [[nodiscard]] std::string get_raw_checksum(const std::filesystem::path& file_path) const override;

    /**
     * @brief Compares two files by their Data Object payload bytes, which
     * recompress() never touches.
     * @param a First file.
     * @param b Second file.
     * @return true if both files' Data Object bytes match exactly.
     */
    [[nodiscard]] bool raw_equal(const std::filesystem::path &a, const std::filesystem::path &b) const override;
};

} // namespace chisel

#endif //CHISEL_ASF_PROCESSOR_HPP
