//
// Created by Giuseppe Francione on 26/08/26.
//

/**
 * @file avi_processor.hpp
 * @brief Defines the IProcessor implementation for AVI (RIFF).
 */

#ifndef CHISEL_AVI_PROCESSOR_HPP
#define CHISEL_AVI_PROCESSOR_HPP

#include "processor.hpp"
#include <array>
#include <string_view>
#include <span>

namespace chisel {

/**
 * @brief Implements IProcessor for AVI (RIFF).
 *
 * recompress() never touches the 'movi' LIST (the actual audio/video
 * payload) or its internal layout -- only top-level siblings around it:
 * dropping JUNK chunks (also one level inside 'hdrl'), and, unless
 * options.preserve_metadata is false, an 'INFO' LIST, then patching the
 * outer RIFF size field.
 *
 * Unlike MP4/ASF, AVI's classic 'idx1' index is genuinely ambiguous by its
 * own specification: AVIOLDINDEX::dwOffset is documented by Microsoft as
 * "relative to the start of the 'movi' list; however, in some AVI files it
 * is given as an offset from the start of the file" -- both conventions
 * exist in real files, and nothing in the file declares which one is in use.
 *
 * This mirrors the calibration ffmpeg's avidec.c uses (avi_read_idx1()):
 * rather than guessing, it locates the true position of the first real
 * chunk in 'movi' directly, compares it to idx1's raw first entry, and uses
 * the difference as a per-file correction applied uniformly to every entry.
 * The same idea, reimplemented here for byte removal rather than decoding:
 * if that calibration lands close to the file-absolute convention, every
 * idx1 entry is shifted like MP4's stco/co64; if it lands close to the
 * movi-relative convention, no shift is needed at all (as long as nothing
 * is ever removed from inside 'movi', which this processor guarantees).
 * If neither reading is a confident match, or an idx1 entry doesn't survive
 * a round-trip sanity check afterward, the file is left untouched.
 */
class AviProcessor final : public IProcessor {
public:
    // --- self-description ---
    [[nodiscard]] std::string_view get_name() const noexcept override {
        return "AviProcessor";
    }

    [[nodiscard]] std::span<const std::string_view> get_supported_mime_types() const noexcept override {
        static constexpr std::array<std::string_view, 2> kMimes = { "video/x-msvideo", "video/avi" };
        return {kMimes.data(), kMimes.size()};
    }

    [[nodiscard]] std::span<const std::string_view> get_supported_extensions() const noexcept override {
        static constexpr std::array<std::string_view, 1> kExts = { ".avi" };
        return {kExts.data(), kExts.size()};
    }

    // --- capabilities ---
    [[nodiscard]] bool can_recompress() const noexcept override { return true; }
    [[nodiscard]] bool can_extract_contents() const noexcept override { return false; }

    // --- operations ---

    /**
     * @brief Strips JUNK chunks (and, unless preserving metadata, the INFO
     * list) from an AVI file, patching the RIFF size and, when needed and
     * safe to do so, the idx1 index.
     * @param input Path to the source file.
     * @param output Path to write the cleaned file.
     * @param options Processing options.
     * @throws std::runtime_error on a malformed or unreadable RIFF structure.
     */
    void recompress(const std::filesystem::path& input,
                    const std::filesystem::path& output, const ProcessingOptions &options) override;

    /**
     * @brief AVI is not treated as a container here.
     * @return std::nullopt
     */
    std::optional<ExtractedContent> prepare_extraction(
        [[maybe_unused]] const std::filesystem::path& input_path) override { return std::nullopt; }

    /**
     * @brief AVI is not treated as a container here.
     * @return Empty path.
     */
    std::filesystem::path finalize_extraction(const ExtractedContent &, const ProcessingOptions &) override { return {}; }

    // --- integrity check ---

    /**
     * @brief (Not implemented) Compute a raw checksum.
     * @return An empty string.
     * @note raw_equal() below does a real structural comparison instead.
     */
    [[nodiscard]] std::string get_raw_checksum(const std::filesystem::path& file_path) const override;

    /**
     * @brief Compares two files by their 'movi' LIST payload bytes, which
     * recompress() never touches or reorders internally.
     * @param a First file.
     * @param b Second file.
     * @return true if both files' 'movi' bytes match exactly.
     */
    [[nodiscard]] bool raw_equal(const std::filesystem::path &a, const std::filesystem::path &b) const override;
};

} // namespace chisel

#endif //CHISEL_AVI_PROCESSOR_HPP
