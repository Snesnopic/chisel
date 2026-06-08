//
// Created by Giuseppe Francione on 19/10/25.
//

/**
 * @file archive_processor.hpp
 * @brief Defines the IProcessor for generic archive formats (ZIP, TAR, RAR, etc.).
 */

#ifndef CHISEL_ARCHIVE_PROCESSOR_HPP
#define CHISEL_ARCHIVE_PROCESSOR_HPP

#include "processor.hpp"
#include "logger.hpp"
#include <array>
#include <string_view>
#include <span>

namespace chisel {

/**
 * @brief Implements IProcessor for various archive formats using libarchive.
 *
 * @details This processor acts as a generic handler for archive
 * files like .zip, .tar, .rar, .cbz, etc. It does not recompress
 * the archive itself but implements the extraction/finalization
 * pipeline, allowing *internal* files to be processed recursively.
 */
class ArchiveProcessor final : public IProcessor {
public:
    // --- self-description ---
    [[nodiscard]] std::string_view get_name() const noexcept override {
        return "ArchiveProcessor";
    }

    [[nodiscard]] std::span<const std::string_view> get_supported_mime_types() const noexcept override {
        static constexpr std::array<std::string_view, 12> kMimes = {
            "application/zip",
            // "application/x-7z-compressed", // 7z write support is limited/complex
            "application/x-zip-compressed",
            "application/x-tar",
            //"application/gzip", // Handled as filters, not primary formats
            "application/x-iso9660-image",
            "application/x-cpio",
            // "application/x-ms-wim", // Write not supported
            "application/java-archive",
            "application/x-xpinstall",
            "application/vnd.android.package-archive",
            "application/vnd.comicbook+zip",
            "application/vnd.comicbook+tar",
            "application/epub+zip",
            "application/x-archive",
        };
        return {kMimes.data(), kMimes.size()};
    }

    [[nodiscard]] std::span<const std::string_view> get_supported_extensions() const noexcept override {
        static constexpr std::array<std::string_view, 58> kExts = {
            ".zip", ".tar", ".tgz", ".iso", ".cpio", ".jar", ".xpi", ".apk",
            ".cbz", ".cbt", ".epub", ".a", ".ar", ".lib",
            ".air", ".bsz", ".cdr", ".csl", ".gallery", ".gallerycollection",
            ".galleryitem", ".grs", ".ipa", ".ipsw", ".ita", ".itz", ".nbk",
            ".notebook", ".oex", ".osk", ".pk3", ".puz", ".stz", ".vlt",
            ".wal", ".wba", ".wmz", ".wsz", ".xap", ".xl", ".xlam", ".xmz",
            ".xsn", ".kmz", ".vsix", ".nupkg", ".appx", ".bar", ".dwf", ".easm",
            ".rmskin", ".sldx", ".zipx", ".gz", ".deb", ".ipk", ".svgz"
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
     * @brief This processor extracts files from archives.
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
     * @brief Extracts all files from a supported archive into a temp directory.
     *
     * Uses `archive_read_...` functions (libarchive) to decompress the
     * container and write all entries to a unique temporary directory.
     *
     * @param input_path Path to the archive file (e.g., .zip, .rar).
     * @return An ExtractedContent struct containing the list of
     * extracted files and the temp directory path.
     */
    std::optional<ExtractedContent> prepare_extraction(
        const std::filesystem::path& input_path) override;

    /**
     * @brief Re-builds the archive from the (modified) extracted files.
     *
     * Uses `archive_write_...` (libarchive) to create a new archive.
     * If the original format is not writable (e.g., RAR), it will
     * re-package the contents into a ZIP archive.
     *
     * @param content The ExtractedContent struct from `prepare_extraction`.
     * @param options Processing options (e.g. metadata preservation).
     * @return Path to the newly created temporary archive file.
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

#endif // CHISEL_ARCHIVE_PROCESSOR_HPP