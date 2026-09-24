//
// Created by Giuseppe Francione on 24/09/26.
//

/**
 * @file data_uri_processor.hpp
 * @brief Defines the IProcessor implementation for text files embedding base64 data URIs.
 */

#ifndef CHISEL_DATA_URI_PROCESSOR_HPP
#define CHISEL_DATA_URI_PROCESSOR_HPP

#include "processor.hpp"
#include <array>

namespace chisel {

/**
 * @brief Implements IProcessor for HTML, CSS and Markdown files embedding base64 data URIs.
 *
 * @details Images and fonts embedded as data URIs are extracted for the executor to optimize,
 * and the payloads that got smaller are re-encoded in place. The rest of the text is left
 * byte-identical: the markup itself is never minified.
 */
class DataUriProcessor final : public IProcessor {
public:
    // --- self-description ---
    [[nodiscard]] std::string_view get_name() const noexcept override {
        return "DataUriProcessor";
    }

    [[nodiscard]] std::span<const std::string_view> get_supported_mime_types() const noexcept override {
        static constexpr std::array<std::string_view, 3> kMimes = { "text/html", "text/css", "text/markdown" };
        return {kMimes.data(), kMimes.size()};
    }

    [[nodiscard]] std::span<const std::string_view> get_supported_extensions() const noexcept override {
        static constexpr std::array<std::string_view, 5> kExts = { ".html", ".htm", ".css", ".md", ".markdown" };
        return {kExts.data(), kExts.size()};
    }

    // --- capabilities ---
    [[nodiscard]] bool can_recompress() const noexcept override { return false; }
    [[nodiscard]] bool can_extract_contents() const noexcept override { return true; }

    // --- operations ---

    /**
     * @brief Not supported: only the embedded payloads get optimized.
     */
    void recompress(const std::filesystem::path&,
                    const std::filesystem::path&, const ProcessingOptions&) override {}

    /**
     * @brief Extracts each distinct image or font payload to a file.
     * @param input_path Path to the text file.
     * @return The extracted files, possibly none.
     */
    std::optional<ExtractedContent> prepare_extraction(const std::filesystem::path& input_path) override;

    /**
     * @brief Re-encodes the payloads that got smaller.
     * @param content The ExtractedContent struct from `prepare_extraction`.
     * @param options Processing options.
     * @return Path to the rebuilt file, or an empty path if no payload changed.
     */
    std::filesystem::path finalize_extraction(const ExtractedContent& content, const ProcessingOptions& options) override;

    // --- integrity check ---

    /**
     * @brief Not needed: the processor never recompresses directly.
     * @return An empty string.
     */
    [[nodiscard]] std::string get_raw_checksum(const std::filesystem::path& file_path) const override;
};

} // namespace chisel

#endif // CHISEL_DATA_URI_PROCESSOR_HPP
