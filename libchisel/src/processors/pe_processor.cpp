//
// Created by Giuseppe Francione on 31/05/26.
//

#include "../../include/pe_processor.hpp"
#include "../../include/logger.hpp"
#include "../../include/file_utils.hpp"
#include "../../include/random_utils.hpp"
#include "../../include/file_type.hpp"
#include <fstream>
#include <algorithm>
#include <cstring>

namespace chisel {

uint32_t PeProcessor::rva_to_offset(const uint32_t rva, const std::vector<ImageSectionHeader>& sections) {
    for (const auto& sec : sections) {
        // use std::max(VirtualSize, SizeOfRawData) as the section range can be larger than raw data
        if (rva >= sec.VirtualAddress && rva < sec.VirtualAddress + std::max(sec.VirtualSize, sec.SizeOfRawData)) {
            return sec.PointerToRawData + (rva - sec.VirtualAddress);
        }
    }
    return 0;
}

uint32_t PeProcessor::calculate_pe_checksum(const std::span<const uint8_t> raw_data, const uint32_t pe_pos, const uint16_t magic) {
    uint64_t checksum = 0;
    const size_t size = raw_data.size();
    
    // offset is 64 for pe32, 68 for pe32+
    const uint32_t checksum_offset = pe_pos + 24 + (magic == 0x20B ? 68 : 64);

    for (std::size_t i = 0; i < size; i += 2) {
        if (i == checksum_offset || i == checksum_offset + 2) {
            continue;
        }
        
        uint16_t word = raw_data[i];
        if (i + 1 < size) {
            word |= static_cast<uint16_t>(raw_data[i + 1]) << 8;
        }
        
        checksum += word;
        checksum = (checksum & 0xFFFF) + (checksum >> 16);
    }

    checksum = (checksum & 0xFFFF) + (checksum >> 16);
    return static_cast<uint32_t>(checksum + size);
}

std::optional<ExtractedContent> PeProcessor::prepare_extraction(const std::filesystem::path& input_path) {
    Logger::log(LogLevel::Debug, "starting pe extraction for " + input_path.string(), get_name());

    const auto raw_data = read_file(input_path);
    if (raw_data.size() < 0x40 || raw_data[0] != 'M' || raw_data[1] != 'Z') {
        return std::nullopt;
    }

    uint32_t pe_pos;
    std::memcpy(&pe_pos, raw_data.data() + 0x3C, 4);
    if (pe_pos + 24 + sizeof(ImageFileHeader) > raw_data.size()) {
        return std::nullopt;
    }

    // verify PE signature "PE\0\0"
    if (raw_data[pe_pos] != 'P' || raw_data[pe_pos+1] != 'E') return std::nullopt;

    ImageFileHeader file_header;
    std::memcpy(&file_header, raw_data.data() + pe_pos + 4, sizeof(ImageFileHeader));

    uint16_t opt_magic;
    std::memcpy(&opt_magic, raw_data.data() + pe_pos + 24, 2);

    // PE32 (0x10B) vs PE32+ (0x20B, 64-bit)
    // the NumberOfRvaAndSizes field is at offset 92 in PE32 and 108 in PE32+
    uint32_t num_rva_offset = (opt_magic == 0x20B) ? pe_pos + 24 + 108 : pe_pos + 24 + 92;
    uint32_t data_dir_offset = (opt_magic == 0x20B) ? pe_pos + 24 + 112 : pe_pos + 24 + 96;

    if (num_rva_offset + 4 > raw_data.size()) return std::nullopt;

    uint32_t num_rva;
    std::memcpy(&num_rva, raw_data.data() + num_rva_offset, 4);
    if (num_rva <= 2) return std::nullopt; // we need at least index 2 (Resource Directory)

    ImageDataDirectory rsrc_dir_info;
    if (data_dir_offset + (3 * sizeof(ImageDataDirectory)) > raw_data.size()) return std::nullopt;
    std::memcpy(&rsrc_dir_info, raw_data.data() + data_dir_offset + (2 * sizeof(ImageDataDirectory)), sizeof(ImageDataDirectory));
    
    if (rsrc_dir_info.VirtualAddress == 0 || rsrc_dir_info.Size == 0) return std::nullopt;

    uint32_t section_table_pos = pe_pos + 4 + sizeof(ImageFileHeader) + file_header.SizeOfOptionalHeader;
    if (section_table_pos + (file_header.NumberOfSections * sizeof(ImageSectionHeader)) > raw_data.size()) {
        return std::nullopt;
    }

    std::vector<ImageSectionHeader> sections(file_header.NumberOfSections);
    std::memcpy(sections.data(), raw_data.data() + section_table_pos, sections.size() * sizeof(ImageSectionHeader));

    uint32_t rsrc_raw_offset = rva_to_offset(rsrc_dir_info.VirtualAddress, sections);
    if (rsrc_raw_offset == 0) return std::nullopt;

    std::vector<RsrcEntry> rsrc_entries;
    traverse_rsrc(raw_data, rsrc_raw_offset, 0, rsrc_entries, sections);

    if (rsrc_entries.empty()) return std::nullopt;

    ExtractedContent content;
    content.original_path = input_path;
    content.temp_dir = make_temp_dir_for(input_path, "pe");
    content.format = ContainerFormat::Pe;

    for (std::size_t i = 0; i < rsrc_entries.size(); ++i) {
        const auto& entry = rsrc_entries[i];
        uint32_t data_file_offset = rva_to_offset(entry.data_entry.OffsetToData, sections);
        
        if (data_file_offset == 0 || data_file_offset + entry.data_entry.Size > raw_data.size()) continue;

        std::string ext = ".bin";
        const uint8_t* p = raw_data.data() + data_file_offset;
        if (entry.data_entry.Size > 4) {
            if (p[0] == 0x89 && p[1] == 'P') ext = ".png";
            else if (p[0] == 0xFF && p[1] == 0xD8) ext = ".jpg";
            else if (p[0] == 'G' && p[1] == 'I') ext = ".gif";
            else if (p[0] == 'B' && p[1] == 'M') ext = ".bmp";
        }

        std::filesystem::path out_p = content.temp_dir / (std::to_string(i) + "_" + entry.name + ext);
        std::ofstream out_file(out_p, std::ios::binary);
        out_file.write(reinterpret_cast<const char*>(p), entry.data_entry.Size);
        out_file.close();

        content.extracted_files.push_back(out_p);
    }

    content.extras = std::make_any<std::vector<RsrcEntry>>(rsrc_entries);
    return content;
}

void PeProcessor::traverse_rsrc(const std::vector<uint8_t>& data, const uint32_t rsrc_base_offset, const uint32_t dir_offset, 
                               std::vector<RsrcEntry>& rsrc_data, const std::vector<ImageSectionHeader>& sections,
                               const std::string &name, const int depth) {
    if (depth > 10) return; // prevent stack overflow on circular references

    if (rsrc_base_offset + dir_offset + sizeof(ImageResourceDirectory) > data.size()) return;

    ImageResourceDirectory dir;
    std::memcpy(&dir, data.data() + rsrc_base_offset + dir_offset, sizeof(ImageResourceDirectory));

    const uint32_t entry_start = rsrc_base_offset + dir_offset + sizeof(ImageResourceDirectory);
    const int total_entries = dir.NumberOfNamedEntries + dir.NumberOfIdEntries;

    for (int i = 0; i < total_entries; ++i) {
        const uint32_t current_entry_pos = entry_start + (i * sizeof(ImageResourceDirectoryEntry));
        if (current_entry_pos + sizeof(ImageResourceDirectoryEntry) > data.size()) break;

        ImageResourceDirectoryEntry entry;
        std::memcpy(&entry, data.data() + current_entry_pos, sizeof(ImageResourceDirectoryEntry));

        const bool is_dir = (entry.OffsetToDataOrDirectory & 0x80000000) != 0;
        const uint32_t offset = entry.OffsetToDataOrDirectory & 0x7FFFFFFF;

        if (is_dir) {
            traverse_rsrc(data, rsrc_base_offset, offset, rsrc_data, sections, name + "_" + std::to_string(i), depth + 1);
        } else {
            if (rsrc_base_offset + offset + sizeof(ImageResourceDataEntry) <= data.size()) {
                RsrcEntry e;
                std::memcpy(&e.data_entry, data.data() + rsrc_base_offset + offset, sizeof(ImageResourceDataEntry));
                e.data_entry_offset = rsrc_base_offset + offset;
                e.name = name + "_" + std::to_string(i);
                rsrc_data.push_back(e);
            }
        }
    }
}

std::filesystem::path PeProcessor::finalize_extraction(const ExtractedContent& content, const ProcessingOptions&) {
    Logger::log(LogLevel::Debug, "starting pe finalization for " + content.original_path.string(), get_name());

    auto raw_data = read_file(content.original_path);
    auto rsrc_entries = std::any_cast<std::vector<RsrcEntry>>(content.extras);
    bool modified = false;

    // re-parse sections in local buffer to find physical offsets
    uint32_t pe_pos;
    std::memcpy(&pe_pos, raw_data.data() + 0x3C, 4);
    ImageFileHeader file_header;
    std::memcpy(&file_header, raw_data.data() + pe_pos + 4, sizeof(ImageFileHeader));

    uint32_t section_table_pos = pe_pos + 4 + sizeof(ImageFileHeader) + file_header.SizeOfOptionalHeader;
    std::vector<ImageSectionHeader> sections(file_header.NumberOfSections);
    std::memcpy(sections.data(), raw_data.data() + section_table_pos, sections.size() * sizeof(ImageSectionHeader));

    for (std::size_t i = 0; i < content.extracted_files.size(); ++i) {
        auto opt_data = read_file(content.extracted_files[i]);
        if (opt_data.size() <= rsrc_entries[i].data_entry.Size) {
            uint32_t data_offset = rva_to_offset(rsrc_entries[i].data_entry.OffsetToData, sections);
            if (data_offset != 0 && data_offset + opt_data.size() <= raw_data.size()) {
                // overwrite data in-place (safe as long as new size <= old size)
                std::memcpy(raw_data.data() + data_offset, opt_data.data(), opt_data.size());
                
                // update the size in the Resource Data Entry
                uint32_t new_size = static_cast<uint32_t>(opt_data.size());
                std::memcpy(raw_data.data() + rsrc_entries[i].data_entry_offset + 4, &new_size, 4);
                modified = true;
            }
        }
    }

    if (modified) {
        uint16_t magic;
        std::memcpy(&magic, raw_data.data() + pe_pos + 24, 2);

        // invalidate Authenticode signature to prevent "corrupt file" OS warnings
        uint32_t data_dir_offset = (magic == 0x20B) ? pe_pos + 24 + 112 : pe_pos + 24 + 96;
        uint32_t sec_dir_pos = data_dir_offset + (4 * sizeof(ImageDataDirectory));
        if (sec_dir_pos + 8 <= raw_data.size()) {
            uint32_t zero = 0;
            std::memcpy(raw_data.data() + sec_dir_pos, &zero, 4);     // VirtualAddress = 0
            std::memcpy(raw_data.data() + sec_dir_pos + 4, &zero, 4); // Size = 0
        }

        // recalculate and patch PE checksum with dynamic offset (64 for PE32, 68 for PE32+)
        uint32_t new_checksum = calculate_pe_checksum(raw_data, pe_pos, magic);
        uint32_t checksum_offset = pe_pos + 24 + (magic == 0x20B ? 68 : 64);
        
        if (checksum_offset + 4 <= raw_data.size()) {
            std::memcpy(raw_data.data() + checksum_offset, &new_checksum, 4);
        }

        Logger::log(LogLevel::Debug, "patched pe checksum and invalidated authenticode", get_name());
    }

    std::filesystem::path output_path = std::filesystem::temp_directory_path() /
        (content.original_path.stem().string() + "_final" + RandomUtils::random_suffix() + content.original_path.extension().string());

    std::ofstream out_file(output_path, std::ios::binary);
    out_file.write(reinterpret_cast<const char*>(raw_data.data()), raw_data.size());
    out_file.close();

    cleanup_temp_dir(content.temp_dir, get_name());
    return output_path;
}

bool PeProcessor::raw_equal(const std::filesystem::path& a, const std::filesystem::path& b) const {
    return read_file(a) == read_file(b);
}

std::string PeProcessor::get_raw_checksum(const std::filesystem::path& file_path) const  {
    return "";
}

} // namespace chisel
