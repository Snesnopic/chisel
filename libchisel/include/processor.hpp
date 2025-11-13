//
// Created by Giuseppe Francione on 19/10/25.
//

#ifndef CHISEL_PROCESSOR_HPP
#define CHISEL_PROCESSOR_HPP

#include <filesystem>
#include <vector>
#include <string>
#include <optional>
#include <span>
#include <string_view>
#include <stdexcept>

enum class ContainerFormat;

/**
 * @namespace chisel
 * @brief The main namespace for the Chisel library.
 *
 * @details This namespace encapsulates all core functionality of Chisel,
 * including the abstract IProcessor interface, concrete file processor
 * implementations (e.g., PngProcessor, ArchiveProcessor), the execution
 * engine (ProcessorExecutor), and various utility helpers.
 *
 * All internal classes and functions are contained within this namespace
 * to prevent symbol collisions.
 */
namespace chisel {

/**
 * @brief Holds information about content extracted by a processor.
 *
 * This structure is returned by processors that can extract container
 * contents (e.g. archives, multimedia containers). It provides the
 * temporary directory and the list of extracted files, which will later
 * be reassembled by finalize_extraction().
 */
struct ExtractedContent {
    std::filesystem::path original_path;                ///< Path to the original container file
    std::filesystem::path temp_dir;                     ///< Temporary directory holding extracted files
    std::vector<std::filesystem::path> extracted_files; ///< Absolute paths to extracted files
    ContainerFormat format;                             ///< Format of the container
    // Processors may add custom, format-specific context if needed
};

/**
 * @brief Interface for a file processing module in chisel.
 *
 * Each implementation targets a specific file format (or a group of related formats).
 * It must be self-descriptive about the formats it handles (MIME types, extensions)
 * and declare its capabilities (direct recompression, content extraction).
 *
 * Implementations should be stateless regarding the files being processed.
 * Any required state should be specific to a single operation. The ProcessorRegistry
 * owns processor instances and ensures they are reused safely.
 */
class IProcessor {
public:
    virtual ~IProcessor() = default;

    // --- self-description ---

    /// @return Human-readable name of the processor (e.g. "PNG", "FLAC").
    [[nodiscard]] virtual std::string_view get_name() const noexcept = 0;

    /// @return List of supported MIME types (e.g. "image/png").
    [[nodiscard]] virtual std::span<const std::string_view, std::dynamic_extent>
    get_supported_mime_types() const noexcept = 0;

    /// @return List of supported file extensions (e.g. ".png").
    [[nodiscard]] virtual std::span<const std::string_view, std::dynamic_extent>
    get_supported_extensions() const noexcept = 0;

    // --- capabilities ---

    /// @return True if this processor can perform direct recompression.
    [[nodiscard]] virtual bool can_recompress() const noexcept = 0;

    /// @return True if this processor can extract container contents.
    [[nodiscard]] virtual bool can_extract_contents() const noexcept = 0;

    // --- operations ---

    /**
     * @brief Perform direct, lossless recompression if supported.
     * @param input_path Path to the original file.
     * @param output_path Path where the optimized file should be written.
     * @param preserve_metadata Whether to preserve metadata blocks.
     */
    virtual void recompress(const std::filesystem::path& input_path,
                            const std::filesystem::path& output_path,
                            bool preserve_metadata) = 0;

    /**
     * @brief Extract processable internal contents if supported.
     * @param input_path Path to the container file.
     * @return ExtractedContent with details about extracted files and temp dir,
     *         or std::nullopt if no processable content was found.
     */
    virtual std::optional<ExtractedContent> prepare_extraction(
        const std::filesystem::path& input_path) = 0;

    /**
     * @brief Rebuild the original container file after its contents have been modified.
     * @param content The ExtractedContent struct returned by prepare_extraction().
     * @param target_format Target format, if the current one cannot be re-written.
     * @return Path to the newly created optimized temporary container, or an
     * empty path if finalization failed or was skipped.
     */
    virtual std::filesystem::path finalize_extraction(const ExtractedContent &content,
                                                      ContainerFormat target_format) = 0;

    // --- integrity check ---

    /**
     * @brief Compute a raw checksum of the file.
     * @param file_path Path to the file.
     * @return Checksum string (algorithm is processor-specific).
     */
    [[nodiscard]] virtual std::string get_raw_checksum(const std::filesystem::path& file_path) const = 0;

    /**
     * @brief Compare two files at raw level (using checksums or direct comparison).
     * @param a First file path.
     * @param b Second file path.
     * @return true if the two files are raw-equivalent.
     */
    [[nodiscard]] virtual bool raw_equal(const std::filesystem::path& a,
                                         const std::filesystem::path& b) const {
        return get_raw_checksum(a) == get_raw_checksum(b);
    }
};

} // namespace chisel

#endif // CHISEL_PROCESSOR_HPP