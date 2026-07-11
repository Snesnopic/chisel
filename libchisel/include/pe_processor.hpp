//
// Created by Giuseppe Francione on 31/05/26.
//

/**
 * @file pe_processor.hpp
 * @brief Defines the IProcessor implementation for Windows Portable Executable (PE) files.
 */

#ifndef CHISEL_PE_PROCESSOR_HPP
#define CHISEL_PE_PROCESSOR_HPP

#include "processor.hpp"
#include <vector>
#include <string>
#include <array>
#include <string_view>
#include <span>
#include <cstdint>

namespace chisel {

    /**
     * @brief Implements IProcessor for PE files (EXE, DLL, etc.).
     *
     * @details This processor acts as a container, traversing the PE resource tree
     * to find and optimize embedded images (icons, bitmaps, etc.). It updates
     * RVAs and sizes to maintain binary integrity.
     */
    class PeProcessor final : public IProcessor {
    public:
        // --- self-description ---
        [[nodiscard]] std::string_view get_name() const noexcept override {
            return "PeProcessor";
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_mime_types() const noexcept override {
            static constexpr std::array<std::string_view, 2> kMimes = { 
                "application/x-msdownload", 
                "application/vnd.microsoft.portable-executable" 
            };
            return {kMimes.data(), kMimes.size()};
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_extensions() const noexcept override {
            static constexpr std::array<std::string_view, 5> kExts = { 
                ".exe", ".dll", ".ocx", ".scr", ".cpl" 
            };
            return {kExts.data(), kExts.size()};
        }

        // --- capabilities ---
        [[nodiscard]] bool can_recompress() const noexcept override { return false; }
        [[nodiscard]] bool can_extract_contents() const noexcept override { return true; }

        // --- operations ---

        /**
         * @brief PE is a container-only format for chisel.
         */
        void recompress(const std::filesystem::path&,
                        const std::filesystem::path&, const ProcessingOptions&) override {}

        /**
         * @brief Scans the PE file for embedded resources.
         *
         * @param input_path Path to the source PE file.
         * @return ExtractedContent containing temporary files for each resource found.
         */
        std::optional<ExtractedContent> prepare_extraction(const std::filesystem::path& input_path) override;

        /**
         * @brief Rebuilds the PE file with optimized resources.
         *
         * @param content The extracted content structure.
         * @param options Processing options.
         * @return Path to the rebuilt PE file.
         */
        std::filesystem::path finalize_extraction(const ExtractedContent& content, const ProcessingOptions& options) override;

        // --- integrity check ---

        /**
         * @brief Compares two PE files.
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
#pragma pack(push, 1)
        struct ImageFileHeader {
            uint16_t Machine;
            uint16_t NumberOfSections;
            uint32_t TimeDateStamp;
            uint32_t PointerToSymbolTable;
            uint32_t NumberOfSymbols;
            uint16_t SizeOfOptionalHeader;
            uint16_t Characteristics;
        };

        struct ImageDataDirectory {
            uint32_t VirtualAddress;
            uint32_t Size;
        };

        struct ImageSectionHeader {
            uint8_t Name[8];
            uint32_t VirtualSize;
            uint32_t VirtualAddress;
            uint32_t SizeOfRawData;
            uint32_t PointerToRawData;
            uint32_t PointerToRelocations;
            uint32_t PointerToLinenumbers;
            uint16_t NumberOfRelocations;
            uint16_t NumberOfLinenumbers;
            uint32_t Characteristics;
        };

        struct ImageResourceDirectory {
            uint32_t Characteristics;
            uint32_t TimeDateStamp;
            uint16_t MajorVersion;
            uint16_t MinorVersion;
            uint16_t NumberOfNamedEntries;
            uint16_t NumberOfIdEntries;
        };

        struct ImageResourceDirectoryEntry {
            uint32_t NameOrId;
            uint32_t OffsetToDataOrDirectory;
        };

        struct ImageResourceDataEntry {
            uint32_t OffsetToData;
            uint32_t Size;
            uint32_t CodePage;
            uint32_t Reserved;
        };
#pragma pack(pop)

        struct RsrcEntry {
            ImageResourceDataEntry data_entry;
            std::size_t data_entry_offset; ///< Absolute offset of the ImageResourceDataEntry in the file
            std::string name;
        };

        /// @brief Parsed, validated PE header fields shared by prepare_extraction() and finalize_extraction().
        struct PeLayout {
            uint32_t pe_pos;
            uint16_t magic;             ///< 0x10B (PE32) or 0x20B (PE32+)
            uint32_t data_dir_offset;   ///< absolute file offset of DataDirectory[0]
            uint32_t num_data_dir;
            uint32_t file_alignment;
            uint32_t section_alignment;
            uint32_t section_table_pos; ///< absolute file offset of the section header table
            ImageFileHeader file_header;
            std::vector<ImageSectionHeader> sections;
        };

        /**
         * @brief Calculates the PE checksum using the standard 16-bit 1s-complement algorithm.
         */
        [[nodiscard]] static uint32_t calculate_pe_checksum(std::span<const uint8_t> raw_data, uint32_t pe_pos);

        /**
         * @brief Translates a Relative Virtual Address (RVA) to a physical file offset.
         */
        [[nodiscard]] static uint32_t rva_to_offset(uint32_t rva, const std::vector<ImageSectionHeader>& sections);

        /**
         * @brief Recursively traverses the PE resource directory tree.
         */
        static void traverse_rsrc(const std::vector<uint8_t>& data, uint32_t rsrc_base_offset,
                                  uint32_t dir_offset, std::vector<RsrcEntry>& rsrc_data,
                                  const std::vector<ImageSectionHeader>& sections,
                                  const std::string &name = "", int depth = 0);

        /**
         * @brief Parses and validates the DOS/PE/optional headers and section table.
         * @return std::nullopt if the file isn't a valid PE, or bounds checks fail.
         */
        [[nodiscard]] static std::optional<PeLayout> parse_layout(const std::vector<uint8_t>& raw_data);

        /**
         * @brief Finds the section table index whose VirtualAddress matches the given RVA range start.
         * @return -1 if not found.
         */
        [[nodiscard]] static int find_section_by_rva(const PeLayout& layout, uint32_t rva);
    };

} // namespace chisel

#endif // CHISEL_PE_PROCESSOR_HPP
