//
// Created by Giuseppe Francione on 31/05/26.
//

/**
 * @file vcf_processor.hpp
 * @brief Defines the IProcessor implementation for VCF (vCard) files.
 */

#ifndef CHISEL_VCF_PROCESSOR_HPP
#define CHISEL_VCF_PROCESSOR_HPP

#include "processor.hpp"
#include <vector>
#include <string>
#include <array>
#include <string_view>
#include <span>

namespace chisel {

    /**
     * @brief Implements IProcessor for VCF (vCard) files.
     *
     * @details This processor acts as a container, extracting Base64 encoded
     * photos from VCF files, allowing them to be optimized by other processors,
     * and re-integrating them into the VCF file.
     */
    class VcfProcessor final : public IProcessor {
    public:
        // --- self-description ---
        [[nodiscard]] std::string_view get_name() const noexcept override {
            return "VcfProcessor";
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_mime_types() const noexcept override {
            static constexpr std::array<std::string_view, 2> kMimes = { "text/vcard", "text/x-vcard" };
            return {kMimes.data(), kMimes.size()};
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_extensions() const noexcept override {
            static constexpr std::array<std::string_view, 1> kExts = { ".vcf" };
            return {kExts.data(), kExts.size()};
        }

        // --- capabilities ---
        [[nodiscard]] bool can_recompress() const noexcept override { return false; }
        [[nodiscard]] bool can_extract_contents() const noexcept override { return true; }

        // --- operations ---

        /**
         * @brief vCard is a container-only format for chisel.
         */
        void recompress(const std::filesystem::path&,
                        const std::filesystem::path&, const ProcessingOptions&) override {}

        /**
         * @brief Scans the VCF file for Base64 encoded photos.
         *
         * @param input_path Path to the source vCard file.
         * @return ExtractedContent containing temporary files for each photo found.
         */
        std::optional<ExtractedContent> prepare_extraction(const std::filesystem::path& input_path) override;

        /**
         * @brief Rebuilds the VCF file with optimized photos.
         *
         * @param content The extracted content structure.
         * @param options Processing options.
         * @return Path to the rebuilt VCF file.
         */
        std::filesystem::path finalize_extraction(const ExtractedContent& content, const ProcessingOptions& options) override;

        // --- integrity check ---

        /**
         * @brief Compares two VCF files.
         *
         * @param a Path to the first file.
         * @param b Path to the second file.
         * @return true if files are byte-identical.
         */
        [[nodiscard]] bool raw_equal(const std::filesystem::path& a, const std::filesystem::path& b) const override;

        /**
         * @brief (Not Implemented) Compute a raw checksum.
         * @return An empty string.
         */
        [[nodiscard]] std::string get_raw_checksum(const std::filesystem::path& file_path) const override;

    private:
        /**
         * @brief Stores the position of a photo within the VCF text.
         */
        struct PhotoPosition {
            std::size_t start;       ///< start of the Base64 string
            std::size_t end;         ///< end of the Base64 string
            std::string prefix; ///< the PHOTO;...: part
        };
    };

} // namespace chisel

#endif // CHISEL_VCF_PROCESSOR_HPP
