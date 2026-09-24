//
// Created by Giuseppe Francione on 22/02/26.
//

/**
 * @file xml_processor.hpp
 * @brief defines the IProcessor implementation for XML files.
 */

#ifndef CHISEL_XML_PROCESSOR_HPP
#define CHISEL_XML_PROCESSOR_HPP

#include "processor.hpp"
#include <array>

namespace chisel {

/**
 * @brief implements IProcessor for XML files.
 *
 * @details minifies by dropping whitespace inside tags and around the root element,
 * leaving text, attribute values, comments, CDATA and the DOCTYPE byte-identical,
 * and recompresses base64 images embedded in attributes as data URIs.
 * Documents whose encoding isn't ASCII-compatible (e.g. UTF-16) are left untouched.
 */
class XmlProcessor : public IProcessor {
public:
    // --- self-description ---

    [[nodiscard]] std::string_view get_name() const noexcept override {
        return "XmlProcessor";
    }

    [[nodiscard]] std::span<const std::string_view, std::dynamic_extent>
    get_supported_mime_types() const noexcept override {
        static constexpr std::array<std::string_view, 12> mimes = {
            "application/xml",
            "text/xml",
            "text/xsl",
            "application/xhtml+xml",
            "image/svg+xml",
            "application/vnd.google-earth.kml+xml",
            "application/gpx+xml",
            "model/vnd.collada+xml",
            "application/rss+xml",
            "application/atom+xml",
            "application/rdf+xml",
            "application/x-fictionbook+xml"
        };
        return {mimes};
    }

    [[nodiscard]] std::span<const std::string_view, std::dynamic_extent>
    get_supported_extensions() const noexcept override {
        static constexpr std::array<std::string_view, 12> exts = {
            ".xml",
            ".xhtml",
            ".svg",
            ".kml",
            ".gpx",
            ".dae",
            ".rss",
            ".atom",
            ".xmp",
            ".fb2",
            ".xsl",
            ".xslt"
        };
        return {exts};
    }

    // --- capabilities ---

    [[nodiscard]] bool can_recompress() const noexcept override { return true; }
    [[nodiscard]] bool can_extract_contents() const noexcept override { return true; }

    // --- operations ---

    /**
     * @brief writes the minified XML, or a copy of the input if it can't be minified safely.
     * @param input_path path to the original XML file.
     * @param output_path path where the minified XML should be written.
     * @param options Processing options.
     */
    void recompress(const std::filesystem::path& input_path,
                    const std::filesystem::path& output_path, const ProcessingOptions &options) override;

    /**
     * @brief extracts base64 encoded assets (e.g. data:image/png;base64,...) into temporary files.
     * @param input_path path to the XML file.
     * @return ExtractedContent containing paths to extracted binary assets, or std::nullopt.
     */
    std::optional<ExtractedContent> prepare_extraction(
        const std::filesystem::path& input_path) override;

    /**
     * @brief puts the assets that got smaller back into their data URIs.
     * @param content the ExtractedContent struct with processor state and file paths.
     * @param options Processing options (e.g. metadata preservation).
     * @return path to the rebuilt XML file, or an empty path if no asset changed.
     */
    std::filesystem::path finalize_extraction(const ExtractedContent& content, const ProcessingOptions &options) override;

    // --- integrity check ---

    /**
     * @brief not meaningful for XML, returns an empty string.
     * @param file_path path to the XML file.
     * @return empty string.
     */
    [[nodiscard]] std::string get_raw_checksum(const std::filesystem::path& file_path) const override;

    /**
     * @brief checks that two XML files have the same nodes, text and attribute values, compared verbatim.
     * @param a first XML file.
     * @param b second XML file.
     * @return true if the documents are equivalent.
     */
    [[nodiscard]] bool raw_equal(const std::filesystem::path& a,
                                 const std::filesystem::path& b) const override;
};

} // namespace chisel

#endif // CHISEL_XML_PROCESSOR_HPP