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
#include <pugixml.hpp>
#include <memory>
#include <vector>
#include <utility>
#include <array>

namespace chisel {

/**
 * @brief holds dom and mappings for extracted base64 files.
 */
struct xml_state {
    std::shared_ptr<pugi::xml_document> doc;
    std::vector<std::pair<std::filesystem::path, pugi::xml_node>> mappings;
};

/**
 * @brief implements IProcessor for XML files using pugixml.
 *
 * @details parses XML documents to perform minification (recompression)
 * and handles the extraction and reinjection of embedded base64 assets
 * (like images in data URIs).
 */
class XmlProcessor : public IProcessor {
public:
    // --- self-description ---

    [[nodiscard]] std::string_view get_name() const noexcept override {
        return "XmlProcessor";
    }

    [[nodiscard]] std::span<const std::string_view, std::dynamic_extent>
    get_supported_mime_types() const noexcept override {
        static constexpr std::array<std::string_view, 10> mimes = {
            "application/xml",
            "text/xml",
            "application/xhtml+xml",
            "image/svg+xml",
            "application/vnd.google-earth.kml+xml",
            "application/gpx+xml",
            "model/vnd.collada+xml",
            "application/rss+xml",
            "application/atom+xml",
            "application/rdf+xml"
        };
        return {mimes};
    }

    [[nodiscard]] std::span<const std::string_view, std::dynamic_extent>
    get_supported_extensions() const noexcept override {
        static constexpr std::array<std::string_view, 9> exts = {
            ".xml",
            ".xhtml",
            ".svg",
            ".kml",
            ".gpx",
            ".dae",
            ".rss",
            ".atom",
            ".xmp"
        };
        return {exts};
    }

    // --- capabilities ---

    [[nodiscard]] bool can_recompress() const noexcept override { return true; }
    [[nodiscard]] bool can_extract_contents() const noexcept override { return true; }

    // --- operations ---

    /**
     * @brief recompresses an XML file by parsing and saving it in raw format (minified).
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
     * @brief re-encodes previously extracted and processed assets back to base64 into the dom.
     * @param content the ExtractedContent struct with processor state and file paths.
     * @param options Processing options (e.g. metadata preservation).
     * @return path to the newly created minified XML file containing the updated assets.
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
     * @brief checks if two XML files are semantically equivalent by stripping formatting.
     * @param a first XML file.
     * @param b second XML file.
     * @return true if the raw dom representation is identical.
     */
    [[nodiscard]] bool raw_equal(const std::filesystem::path& a,
                                 const std::filesystem::path& b) const override;
};

} // namespace chisel

#endif // CHISEL_XML_PROCESSOR_HPP