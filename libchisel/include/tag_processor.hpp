//
// Created by Giuseppe Francione on 11/07/26.
//

/**
 * @file tag_processor.hpp
 * @brief Defines the IProcessor implementation for cover-art-only audio/video containers.
 */

#ifndef CHISEL_TAG_PROCESSOR_HPP
#define CHISEL_TAG_PROCESSOR_HPP

#include "processor.hpp"
#include <array>
#include <string_view>
#include <span>

namespace chisel {

    /**
     * @brief Implements IProcessor for audio/video containers where chisel only
     * extracts and re-optimizes embedded cover art -- the underlying audio/video
     * stream itself is never recompressed.
     *
     * @details Covers AIFF, ASF/WMA/WMV, DSDIFF, DSF, MP4/M4A/MOV/3GP, WAV,
     * Musepack, and TrueAudio. All of these delegate to the same TagLib-based
     * AudioMetadataUtil extraction/reinsertion logic, which already dispatches
     * internally by the concrete TagLib::File subtype -- so one processor
     * covering every one of these formats is exactly as correct as maintaining
     * eight separate near-identical classes, just with one shared integrity
     * check instead of eight copies that never checked anything at all.
     */
    class TagProcessor final : public IProcessor {
    public:
        // --- self-description ---
        [[nodiscard]] std::string_view get_name() const noexcept override {
            return "TagProcessor";
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_mime_types() const noexcept override {
            static constexpr std::array<std::string_view, 23> kMimes = {
                // AIFF
                "audio/x-aiff", "audio/aiff",
                // ASF / WMA / WMV
                "audio/x-ms-wma", "video/x-ms-wmv", "video/x-ms-asf",
                // DSDIFF
                "audio/dff", "audio/x-dff",
                // DSF
                "audio/dsf", "audio/x-dsf",
                // MP4 / M4A / MOV / 3GP
                "audio/mp4", "audio/x-m4a", "video/mp4", "video/quicktime", "video/3gpp", "video/3gpp2",
                // WAV
                "audio/wav", "audio/x-wav", "audio/vnd.wave", "audio/wave",
                // Musepack
                "audio/musepack", "audio/x-musepack",
                // TrueAudio
                "audio/tta", "audio/x-tta"
            };
            return {kMimes.data(), kMimes.size()};
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_extensions() const noexcept override {
            static constexpr std::array<std::string_view, 21> kExts = {
                ".aif", ".aiff", ".aifc",                                          // AIFF
                ".wma", ".wmv", ".asf",                                            // ASF / WMA / WMV
                ".dff",                                                            // DSDIFF
                ".dsf",                                                            // DSF
                ".mp4", ".m4a", ".m4b", ".m4v", ".mov", ".qt", ".3gp", ".3g2",     // MP4 family
                ".wav",                                                            // WAV
                ".mpc", ".mp+", ".mpp",                                            // Musepack
                ".tta"                                                             // TrueAudio
            };
            return {kExts.data(), kExts.size()};
        }

        // --- capabilities ---
        [[nodiscard]] bool can_recompress() const noexcept override { return false; }
        [[nodiscard]] bool can_extract_contents() const noexcept override { return true; }

        // --- operations ---

        /**
         * @brief Recompresses one of the covered container formats.
         * @param input Path to the source file.
         * @param output Path to write the optimized file.
         * @param options Processing options.
         */
        void recompress(const std::filesystem::path& input,
                        const std::filesystem::path& output, const ProcessingOptions &options) override;

        /**
         * @brief Prepares extraction of embedded cover art.
         * @param input_path Path to the source file.
         * @return ExtractedContent struct or nullopt if no cover art was found.
         */
        std::optional<ExtractedContent> prepare_extraction(
            const std::filesystem::path& input_path) override;

        /**
         * @brief Rebuilds the file from extracted cover art content.
         * @param content The ExtractedContent struct.
         * @param options Processing options.
         * @return Path to the finalized file.
         */
        std::filesystem::path finalize_extraction(const ExtractedContent &content, const ProcessingOptions &options) override;

        // --- integrity check ---

        /**
         * @brief Not implemented: can_recompress() == false means the executor
         * never calls raw_equal() on this processor (only Phase 2's recompress()
         * dispatch does, per processor_executor.cpp), so this and the inherited
         * default raw_equal() are unreachable
         */
        [[nodiscard]] std::string get_raw_checksum(const std::filesystem::path& file_path) const override { return ""; }
    };

} // namespace chisel

#endif // CHISEL_TAG_PROCESSOR_HPP
