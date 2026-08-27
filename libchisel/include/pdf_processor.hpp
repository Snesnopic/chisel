//
// Created by Giuseppe Francione on 19/10/25.
//

/**
 * @file pdf_processor.hpp
 * @brief Defines the IProcessor implementation for PDF files using qpdf.
 */

#ifndef CHISEL_PDF_PROCESSOR_HPP
#define CHISEL_PDF_PROCESSOR_HPP

#include "processor.hpp"
#include <array>
#include <string_view>
#include <span>
#include <unordered_map>
#include <filesystem>
#include <vector>

namespace chisel {

/**
 * @brief Implements IProcessor for PDF files using qpdf and Zopfli.
 *
 * @details This processor has a hybrid role:
 * 1. (Container) It extracts raw PDF streams (images, fonts, etc.)
 * to a temp directory, allowing other processors to optimize them.
 * 2. (Recompressor) During finalization, it re-compresses any
 * internal Flate streams (like text or vector data) using Zopfli.
 *
 * It uses `qpdf` for all PDF parsing and manipulation.
 */
class PdfProcessor final : public IProcessor {
public:
    [[nodiscard]] std::string_view get_name() const noexcept override {
        return "PdfProcessor";
    }

    [[nodiscard]] std::span<const std::string_view> get_supported_mime_types() const noexcept override {
        static constexpr std::array<std::string_view,1> kMimes = { "application/pdf" };
        return {kMimes.data(), kMimes.size()};
    }

    [[nodiscard]] std::span<const std::string_view> get_supported_extensions() const noexcept override {
        static constexpr std::array<std::string_view,1> kExts = { ".pdf" };
        return {kExts.data(), kExts.size()};
    }

    /**
     * @brief This processor does not implement recompress(); its recompression
     * logic runs entirely inside finalize_extraction() instead.
     * @return false
     */
    [[nodiscard]] bool can_recompress() const noexcept override { return false; }
    /**
     * @brief This processor extracts embedded streams.
     * @return true
     */
    [[nodiscard]] bool can_extract_contents() const noexcept override { return true; }

    /**
     * @brief (Not Implemented) Intentionally empty.
     *
     * Recompression logic for PDFs is handled entirely within
     * `finalize_extraction` because it's intertwined with stream
     * extraction and re-assembly.
     */
    void recompress(const std::filesystem::path&,
                    const std::filesystem::path&, const ProcessingOptions &options) override {
        // intentionally empty: PDF recompression is handled in finalize_extraction
    }

    /**
     * @brief Extracts all decodable streams from a PDF file.
     *
     * Uses `qpdf` to iterate through all objects. Any object that is
     * a decodable stream (image, font, text) is uncompressed and
     * saved to a file in a temporary directory.
     *
     * @param input_path Path to the PDF file.
     * @return An ExtractedContent struct populated with paths to the
     * raw, extracted stream files.
     */
    std::optional<ExtractedContent> prepare_extraction(const std::filesystem::path& input_path) override;

    /**
     * @brief Rebuilds the PDF with optimized streams.
     *
     * This method does two things:
     * 1. Re-embeds streams (like images) that were optimized in the
     * temp directory by other processors.
     * 2. Re-compresses internal Flate streams (text, vector graphics)
     * using Zopfli for better compression.
     *
     * Uses `QPDFWriter` to write a new, linearized, and optimized PDF.
     *
     * @param content The ExtractedContent struct from `prepare_extraction`.
     * @param options Processing options (e.g. metadata preservation).
     * @return Path to the newly created temporary PDF file.
     * @throws std::runtime_error if qpdf fails.
     */
    std::filesystem::path finalize_extraction(const ExtractedContent &content, const ProcessingOptions &options) override;

    /**
     * @brief Compares two PDF files by their decoded stream content.
     *
     * Iterates through all objects in both PDF files using qpdf, extracts
     * the *decoded* data for each stream (falling back to raw data for
     * non-decodable streams), sorts each file's collection, and checks that
     * every stream in a survives in b. Decoded content is compared rather
     * than raw bytes since recompression legitimately changes a stream's raw
     * bytes (e.g. re-flating with zopfli) without changing its content;
     * streams are compared as an order-independent collection rather than
     * keyed by object ID, since operations like linearization freely
     * renumber objects between the original and finalized file. b may
     * contain streams a doesn't (as a multiset-subset, not exact equality):
     * QPDFWriter's own linearization unconditionally adds a hint stream that
     * isn't real document content, so requiring exact equality would reject
     * every successfully linearized file.
     *
     * @param a Path to the first PDF file.
     * @param b Path to the second PDF file.
     * @return true if every decoded stream in a is present (with at least
     * the same multiplicity) in b, false otherwise.
     */
    [[nodiscard]] bool raw_equal(const std::filesystem::path &a, const std::filesystem::path &b) const override;

    /**
     * @brief (Not Implemented) Compute a raw checksum.
     * @param file_path Path to the file.
     * @return An empty string.
     */
    [[nodiscard]] std::string get_raw_checksum(const std::filesystem::path& file_path) const override;

private:
    /**
     * @brief Holds metadata about an extracted PDF stream.
     */
    struct StreamInfo {
        bool decodable = false;       ///< True if qpdf could decode the stream
        bool has_decode_parms = false;///< True if stream has /DecodeParms
        std::filesystem::path file;   ///< Path to the extracted raw stream data
        uintmax_t original_size = 0;  ///< Tracking for injection criteria
    };

    /**
     * @brief Holds the state of a PDF extraction process.
     */
    struct PdfState {
        std::unordered_map<int, StreamInfo> streams; ///< Info for each object index
        std::filesystem::path temp_dir;  ///< The temp dir holding extracted streams
    };
};

} // namespace chisel

#endif // CHISEL_PDF_PROCESSOR_HPP