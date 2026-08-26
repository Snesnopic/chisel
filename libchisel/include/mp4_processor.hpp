//
// Created by Giuseppe Francione on 25/08/26.
//

/**
 * @file mp4_processor.hpp
 * @brief Defines the IProcessor implementation for the MP4/QuickTime (ISOBMFF) family.
 */

#ifndef CHISEL_MP4_PROCESSOR_HPP
#define CHISEL_MP4_PROCESSOR_HPP

#include "processor.hpp"
#include <array>
#include <string_view>
#include <span>

namespace chisel {

/**
 * @brief Implements IProcessor for the MP4/QuickTime family (ISOBMFF).
 *
 * recompress() never touches the encoded audio/video bitstream (mdat) -- only
 * the box structure around it: dropping free/skip/wide padding atoms, and
 * (when options.preserve_metadata is false) udta/meta metadata atoms, then
 * patching every parent box's declared size and every stco/co64 chunk-offset
 * table entry to account for the removed bytes. Fragmented files (any moof
 * box present) are left untouched -- the offset bookkeeping fragmentation
 * needs is out of scope here.
 *
 * Cover art extraction/reinsertion uses the same shared AudioMetadataUtil
 * logic TagProcessor uses for every other cover-art-only container; this
 * processor replaces TagProcessor for the MP4 family specifically, since
 * ProcessorExecutor only ever consults the first registered processor for a
 * given format for both capabilities, not a chain of complementary ones.
 */
class Mp4Processor final : public IProcessor {
public:
    // --- self-description ---
    [[nodiscard]] std::string_view get_name() const noexcept override {
        return "Mp4Processor";
    }

    [[nodiscard]] std::span<const std::string_view> get_supported_mime_types() const noexcept override {
        static constexpr std::array<std::string_view, 7> kMimes = {
            "audio/mp4", "audio/x-m4a", "video/mp4", "video/x-m4v", "video/quicktime", "video/3gpp", "video/3gpp2"
        };
        return {kMimes.data(), kMimes.size()};
    }

    [[nodiscard]] std::span<const std::string_view> get_supported_extensions() const noexcept override {
        static constexpr std::array<std::string_view, 8> kExts = {
            ".mp4", ".m4a", ".m4b", ".m4v", ".mov", ".qt", ".3gp", ".3g2"
        };
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
     * @brief Strips padding (and, unless preserving metadata, udta/meta atoms)
     * from an MP4/QuickTime file, patching container sizes and chunk-offset
     * tables to match. Fragmented files are copied through unchanged.
     * @param input Path to the source file.
     * @param output Path to write the cleaned file.
     * @param options Processing options.
     * @throws std::runtime_error on a malformed or unreadable box structure.
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
     * @brief Compares two files by their mdat payload bytes and per-track
     * sample/chunk counts, without touching the box structure otherwise.
     * @param a First file.
     * @param b Second file.
     * @return true if every mdat's bytes and every track's sample/chunk
     * counts match exactly.
     */
    [[nodiscard]] bool raw_equal(const std::filesystem::path &a, const std::filesystem::path &b) const override;
};

} // namespace chisel

#endif //CHISEL_MP4_PROCESSOR_HPP
