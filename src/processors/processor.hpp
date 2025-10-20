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

namespace chisel {

// structure to hold information about content extracted by a processor
struct ExtractedContent {
    std::filesystem::path original_path;                // path to the original container file that was processed
    std::filesystem::path temp_dir;                     // dedicated temporary directory holding the extracted files
    std::vector<std::filesystem::path> extracted_files; // list of absolute paths to the files extracted into temp_dir
    ContainerFormat format;
    // processors can add custom, format-specific context here if needed for finalize_extraction
};

/**
 * @brief interface for a file processing module in chisel.
 *
 * each implementation targets a specific file format (or a group of related formats).
 * it must be self-descriptive about the formats it handles (mime types, extensions)
 * and declare its capabilities (direct recompression, content extraction).
 * it provides the core methods to perform these optimization operations.
 *
 * implementations should be stateless regarding the files being processed. any required
 * state should be specific to single operation being performed. the ProcessorRegistry
 * creates a new instance for each operation to ensure thread safety and isolation.
 */
class IProcessor {
public:
    virtual ~IProcessor() = default;

    // --- self-description ---

    [[nodiscard]] virtual std::string_view get_name() const noexcept = 0;

    [[nodiscard]] virtual std::span<const std::string_view, std::dynamic_extent>
    get_supported_mime_types() const noexcept = 0;

    [[nodiscard]] virtual std::span<const std::string_view, std::dynamic_extent>
    get_supported_extensions() const noexcept = 0;

    // --- capabilities ---

    [[nodiscard]] virtual bool can_recompress() const noexcept = 0;
    [[nodiscard]] virtual bool can_extract_contents() const noexcept = 0;

    // --- operations ---

    /**
     * @brief performs direct, lossless recompression if supported (can_recompress() is true).
     * @param input_path path to the original file.
     * @param output_path path where the optimized file should be written.
     * @param preserve_metadata whether to preserve metadata blocks.
     */
    virtual void recompress(const std::filesystem::path& input_path,
                            const std::filesystem::path& output_path,
                            bool preserve_metadata) = 0;

    /**
     * @brief extracts processable internal contents if supported (can_extract_contents() is true).
     * @param input_path path to the container file.
     * @return std::optional<ExtractedContent> with details about extracted files and temp dir,
     *         or std::nullopt if no processable content was found.
     */
    virtual std::optional<ExtractedContent> prepare_extraction(
        const std::filesystem::path& input_path) = 0;

    /**
     * @brief rebuilds the original container file after its internal contents have potentially been modified.
     * @param content the ExtractedContent struct returned by prepare_extraction.
     * @param target_format target format, if the current one can't be re-written.
     */
    virtual void finalize_extraction(const ExtractedContent& content,
                                     ContainerFormat target_format) = 0;

    // --- integrity check ---

    [[nodiscard]] virtual std::string get_raw_checksum(const std::filesystem::path& file_path) const = 0;
};

} // namespace chisel

#endif // CHISEL_PROCESSOR_HPP