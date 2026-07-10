//
// Created by Giuseppe Francione on 19/10/25.
//

/**
 * @file ooxml_processor.hpp
 * @brief Defines the IProcessor implementation for Office Open XML (OOXML) files.
 */

#ifndef CHISEL_OOXML_PROCESSOR_HPP
#define CHISEL_OOXML_PROCESSOR_HPP

#include "processor.hpp"
#include <array>
#include <string_view>
#include <span>

namespace chisel {

/**
 * @brief Implements IProcessor for Office Open XML (OOXML) files.
 *
 * @details This processor handles .docx, .xlsx, and .pptx files.
 * It treats them as ZIP archives and extracts their contents; embedded
 * images (PNG/JPG) and other recognized parts are optimized independently
 * by the executor recursing into each extracted file (dispatched to
 * PngProcessor/ZopfliPngProcessor/JpegProcessor/etc. as appropriate).
 * finalize_extraction() just re-zips whatever ends up on disk.
 */
class OOXMLProcessor final : public IProcessor {
public:
    // --- self-description ---
    [[nodiscard]] std::string_view get_name() const noexcept override {
        return "OOXMLProcessor";
    }

    [[nodiscard]] std::span<const std::string_view> get_supported_mime_types() const noexcept override {
        static constexpr std::array<std::string_view, 6> kMimes = {
            "application/vnd.openxmlformats-officedocument.wordprocessingml.document",
            "application/vnd.openxmlformats-officedocument.wordprocessingml.template",
            "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
            "application/vnd.openxmlformats-officedocument.spreadsheetml.template",
            "application/vnd.openxmlformats-officedocument.presentationml.presentation",
            "application/vnd.openxmlformats-officedocument.presentationml.template"
        };
        return {kMimes.data(), kMimes.size()};
    }

    [[nodiscard]] std::span<const std::string_view> get_supported_extensions() const noexcept override {
        static constexpr std::array<std::string_view, 14> kExts = {
            ".docx", ".docm", ".dotm", ".dotx",
            ".xlsx", ".xlsm", ".xltm", ".xltx",
            ".pptx", ".pptm", ".potm", ".potx", ".ppsm", ".ppsx"
        };
        return {kExts.data(), kExts.size()};
    }

    // --- capabilities ---

    /**
     * @brief Direct recompression is not supported (handled via extraction).
     * @return false
     */
    [[nodiscard]] bool can_recompress() const noexcept override { return false; }

    /**
     * @brief This processor extracts files from OOXML archives.
     * @return true
     */
    [[nodiscard]] bool can_extract_contents() const noexcept override { return true; }

    // --- operations ---

    /**
     * @brief (Not Implemented) Direct recompression is not supported.
     */
    void recompress(const std::filesystem::path&,
                    const std::filesystem::path&, const ProcessingOptions &options) override {}

    /**
     * @brief Extracts all files from the OOXML (ZIP) container.
     *
     * Uses libarchive to unzip the .docx/.xlsx/.pptx file into
     * a temporary directory.
     *
     * @param input_path Path to the OOXML file.
     * @return An ExtractedContent struct.
     */
    std::optional<ExtractedContent> prepare_extraction(
        const std::filesystem::path& input_path) override;

    /**
     * @brief Rebuilds the OOXML archive from the (possibly already
     * independently optimized) extracted files.
     *
     * Walks the temp directory and re-zips every file and directory found
     * there (each part having already been optimized in place, if
     * applicable, by the executor's own recursive per-file dispatch)
     * using libarchive at maximum deflate compression.
     *
     * @param content The ExtractedContent struct from `prepare_extraction`.
     * @param options Processing options (e.g. metadata preservation).
     * @return Path to the newly created temporary OOXML file.
     * @throws std::runtime_error if archive creation fails.
     */
    std::filesystem::path finalize_extraction(const ExtractedContent &content, const ProcessingOptions &options) override;

    // --- integrity check ---

    /**
     * @brief (Not Implemented) Compute a raw checksum.
     * @param file_path Path to the file.
     * @return An empty string.
     */
    [[nodiscard]] std::string get_raw_checksum(const std::filesystem::path& file_path) const override;
};

} // namespace chisel

#endif // CHISEL_OOXML_PROCESSOR_HPP