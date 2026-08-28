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

namespace {
    uint32_t round_up(const uint32_t value, const uint32_t alignment) {
        if (alignment == 0) return value;
        return (value + alignment - 1) / alignment * alignment;
    }
} // namespace

uint32_t PeProcessor::rva_to_offset(const uint32_t rva, const std::vector<ImageSectionHeader>& sections) {
    for (const auto& sec : sections) {
        // use std::max(VirtualSize, SizeOfRawData) as the section range can be larger than raw data
        if (rva >= sec.VirtualAddress && rva < sec.VirtualAddress + std::max(sec.VirtualSize, sec.SizeOfRawData)) {
            return sec.PointerToRawData + (rva - sec.VirtualAddress);
        }
    }
    return 0;
}

int PeProcessor::find_section_by_rva(const PeLayout& layout, const uint32_t rva) {
    for (std::size_t i = 0; i < layout.sections.size(); ++i) {
        const auto& sec = layout.sections[i];
        if (rva >= sec.VirtualAddress && rva < sec.VirtualAddress + std::max(sec.VirtualSize, sec.SizeOfRawData)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

uint32_t PeProcessor::calculate_pe_checksum(const std::span<const uint8_t> raw_data, const uint32_t pe_pos) {
    uint64_t checksum = 0;
    const size_t size = raw_data.size();

    // CheckSum sits at the same fixed offset (64) in both PE32 and PE32+ optional headers --
    // the two layouts only diverge starting at the later SizeOfStackReserve field
    const uint32_t checksum_offset = pe_pos + 24 + 64;

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

std::optional<PeProcessor::PeLayout> PeProcessor::parse_layout(const std::vector<uint8_t>& raw_data) {
    if (raw_data.size() < 0x40 || raw_data[0] != 'M' || raw_data[1] != 'Z') {
        return std::nullopt;
    }

    uint32_t pe_pos;
    std::memcpy(&pe_pos, raw_data.data() + 0x3C, 4);
    if (static_cast<uint64_t>(pe_pos) + 24 + sizeof(ImageFileHeader) > raw_data.size()) {
        return std::nullopt;
    }
    if (raw_data[pe_pos] != 'P' || raw_data[pe_pos + 1] != 'E') return std::nullopt;

    PeLayout layout{};
    layout.pe_pos = pe_pos;
    std::memcpy(&layout.file_header, raw_data.data() + pe_pos + 4, sizeof(ImageFileHeader));
    std::memcpy(&layout.magic, raw_data.data() + pe_pos + 24, 2);
    if (layout.magic != 0x10B && layout.magic != 0x20B) return std::nullopt;

    const uint32_t opt_hdr = pe_pos + 24;
    const uint32_t section_align_offset = opt_hdr + 32;
    const uint32_t file_align_offset = opt_hdr + 36;
    if (static_cast<uint64_t>(file_align_offset) + 4 > raw_data.size()) return std::nullopt;
    std::memcpy(&layout.section_alignment, raw_data.data() + section_align_offset, 4);
    std::memcpy(&layout.file_alignment, raw_data.data() + file_align_offset, 4);
    if (layout.file_alignment == 0 || layout.section_alignment == 0) return std::nullopt;

    // NumberOfRvaAndSizes is at offset 92 (PE32) / 108 (PE32+); DataDirectory[] follows immediately
    const uint32_t num_rva_offset = opt_hdr + (layout.magic == 0x20B ? 108 : 92);
    layout.data_dir_offset = opt_hdr + (layout.magic == 0x20B ? 112 : 96);
    if (static_cast<uint64_t>(num_rva_offset) + 4 > raw_data.size()) return std::nullopt;
    std::memcpy(&layout.num_data_dir, raw_data.data() + num_rva_offset, 4);
    if (layout.num_data_dir > 16) layout.num_data_dir = 16; // clamp, mirrors Leanify

    layout.section_table_pos = pe_pos + 4 + static_cast<uint32_t>(sizeof(ImageFileHeader)) + layout.file_header.SizeOfOptionalHeader;
    const uint64_t section_table_end = static_cast<uint64_t>(layout.section_table_pos) +
        static_cast<uint64_t>(layout.file_header.NumberOfSections) * sizeof(ImageSectionHeader);
    if (section_table_end > raw_data.size()) return std::nullopt;

    layout.sections.resize(layout.file_header.NumberOfSections);
    std::memcpy(layout.sections.data(), raw_data.data() + layout.section_table_pos,
                layout.sections.size() * sizeof(ImageSectionHeader));

    return layout;
}

std::optional<ExtractedContent> PeProcessor::prepare_extraction(const std::filesystem::path& input_path) {
    Logger::log(LogLevel::Debug, "starting pe extraction for " + input_path.string(), get_name());

    const auto raw_data = read_file(input_path);
    const auto layout_opt = parse_layout(raw_data);
    if (!layout_opt) return std::nullopt;
    const auto& layout = *layout_opt;

    if (static_cast<uint64_t>(layout.data_dir_offset) + (3 * sizeof(ImageDataDirectory)) > raw_data.size()) {
        return std::nullopt;
    }
    if (layout.num_data_dir <= 2) return std::nullopt; // we need at least index 2 (Resource Directory)

    ImageDataDirectory rsrc_dir_info;
    std::memcpy(&rsrc_dir_info, raw_data.data() + layout.data_dir_offset + (2 * sizeof(ImageDataDirectory)), sizeof(ImageDataDirectory));

    if (rsrc_dir_info.VirtualAddress == 0 || rsrc_dir_info.Size == 0) return std::nullopt;

    const uint32_t rsrc_raw_offset = rva_to_offset(rsrc_dir_info.VirtualAddress, layout.sections);
    if (rsrc_raw_offset == 0) return std::nullopt;

    std::vector<RsrcEntry> all_entries;
    traverse_rsrc(raw_data, rsrc_raw_offset, 0, all_entries, layout.sections);
    if (all_entries.empty()) return std::nullopt;

    ExtractedContent content;
    content.original_path = input_path;
    content.temp_dir = make_temp_dir_for(input_path, "pe");
    content.format = ContainerFormat::Pe;

    // kept in lockstep with content.extracted_files: entries that fail the bounds
    // check below are skipped from BOTH, so index i always refers to the same resource
    // in finalize_extraction (previously extracted_files could end up shorter than the
    // full traversal result, silently misaligning indices from the first skip onward)
    std::vector<RsrcEntry> used_entries;

    for (std::size_t i = 0; i < all_entries.size(); ++i) {
        const auto& entry = all_entries[i];
        uint32_t data_file_offset = rva_to_offset(entry.data_entry.OffsetToData, layout.sections);

        if (data_file_offset == 0 || static_cast<uint64_t>(data_file_offset) + entry.data_entry.Size > raw_data.size()) continue;

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
        used_entries.push_back(entry);
    }

    if (content.extracted_files.empty()) return std::nullopt;

    content.extras = std::make_any<std::vector<RsrcEntry>>(used_entries);
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

        // at the top level, NameOrId is the RT_* resource type. RT_GROUP_ICON (14) /
        // RT_GROUP_CURSOR (12) are just small ID/size directory listings (referencing
        // the real image data in separate RT_ICON/RT_CURSOR entries), never
        // compressible content themselves -- and their first bytes happen to match
        // the ICO/CUR magic closely enough that qadmimes misroutes them to
        // IcoProcessor, which then (correctly) rejects them as not a real standalone
        // ICO/CUR file. Skipping extraction here avoids that false error entirely.
        if (depth == 0) {
            const uint32_t type_id = entry.NameOrId & 0x7FFFFFFF;
            if (type_id == 12 || type_id == 14) continue;
        }

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

    if (rsrc_entries.size() != content.extracted_files.size()) {
        throw std::runtime_error("PeProcessor: resource/extracted-file count mismatch");
    }

    const auto layout_opt = parse_layout(raw_data);
    if (!layout_opt) {
        throw std::runtime_error("PeProcessor: original file is no longer a valid PE");
    }
    PeLayout layout = *layout_opt;

    // collect each entry's final payload: the optimized version if it's smaller,
    // otherwise the untouched original bytes (never accept a larger "optimization")
    struct Payload {
        std::vector<uint8_t> data;
        std::size_t data_entry_offset = 0;
        uint32_t original_offset = 0; // RVA
        uint32_t original_size = 0;
    };
    std::vector<Payload> payloads;
    payloads.reserve(rsrc_entries.size());
    bool any_shrunk = false;

    for (std::size_t i = 0; i < rsrc_entries.size(); ++i) {
        const auto opt_data = read_file(content.extracted_files[i]);
        const auto& e = rsrc_entries[i];

        Payload p;
        p.data_entry_offset = e.data_entry_offset;
        p.original_offset = e.data_entry.OffsetToData;
        p.original_size = e.data_entry.Size;

        if (!opt_data.empty() && opt_data.size() <= e.data_entry.Size) {
            if (opt_data.size() < e.data_entry.Size) any_shrunk = true;
            p.data = opt_data;
        } else {
            const uint32_t off = rva_to_offset(e.data_entry.OffsetToData, layout.sections);
            if (off == 0 || static_cast<uint64_t>(off) + e.data_entry.Size > raw_data.size()) {
                throw std::runtime_error("PeProcessor: resource offset out of range during finalize");
            }
            p.data.assign(raw_data.begin() + off, raw_data.begin() + off + e.data_entry.Size);
        }
        payloads.push_back(std::move(p));
    }

    if (!any_shrunk) {
        // nothing was actually optimized: no point rewriting anything
        return {};
    }

    // locate the resource section
    bool can_compact = true;
    int rsrc_idx = -1;
    ImageDataDirectory rsrc_dir_info{};
    if (static_cast<uint64_t>(layout.data_dir_offset) + 3 * sizeof(ImageDataDirectory) <= raw_data.size()) {
        std::memcpy(&rsrc_dir_info, raw_data.data() + layout.data_dir_offset + 2 * sizeof(ImageDataDirectory), sizeof(ImageDataDirectory));
        rsrc_idx = find_section_by_rva(layout, rsrc_dir_info.VirtualAddress);
    }
    if (rsrc_idx < 0) can_compact = false;

    uint32_t rsrc_virtual_address = 0, rsrc_raw_offset = 0, rsrc_raw_size = 0;
    if (can_compact) {
        rsrc_virtual_address = layout.sections[rsrc_idx].VirtualAddress;
        rsrc_raw_offset = layout.sections[rsrc_idx].PointerToRawData;
        rsrc_raw_size = layout.sections[rsrc_idx].SizeOfRawData;
    }

    // sort by original data RVA to determine physical layout order within the section
    std::sort(payloads.begin(), payloads.end(), [](const Payload& a, const Payload& b) {
        return a.original_offset < b.original_offset;
    });

    // validate this is a "standard", non-overlapping, in-bounds resource layout
    // (mirrors Leanify's IsRSRCValid) -- anything else (packers, obfuscators) falls
    // back to the old, safe in-place-only behavior further below
    uint32_t old_data_end_rva = 0;
    if (can_compact) {
        uint32_t prev_end = 0;
        for (const auto& p : payloads) {
            if (p.original_offset < rsrc_virtual_address ||
                static_cast<uint64_t>(p.original_offset) + p.original_size >
                    static_cast<uint64_t>(rsrc_virtual_address) + layout.sections[rsrc_idx].VirtualSize) {
                can_compact = false;
                break;
            }
            if (p.original_offset < prev_end) { can_compact = false; break; } // overlap
            prev_end = p.original_offset + p.original_size;
        }
        old_data_end_rva = prev_end;

        // no unexplained extra bytes (overlay data) beyond what we accounted for
        if (can_compact) {
            const uint32_t old_data_end_raw = rsrc_raw_offset + (old_data_end_rva - rsrc_virtual_address);
            const uint32_t rsrc_end_raw = rsrc_raw_offset + rsrc_raw_size;
            if (round_up(old_data_end_raw, layout.file_alignment) != rsrc_end_raw) can_compact = false;
        }

        // none of the ImageResourceDataEntry structs themselves may live inside the
        // data region we're about to compact (some packers interleave them) -- if so,
        // moving the data without also relocating these structs would corrupt them
        if (can_compact) {
            const uint32_t region_start_raw = rsrc_raw_offset + (payloads.front().original_offset - rsrc_virtual_address);
            const uint32_t region_end_raw = rsrc_raw_offset + (old_data_end_rva - rsrc_virtual_address);
            for (const auto& e : rsrc_entries) {
                if (e.data_entry_offset >= region_start_raw && e.data_entry_offset < region_end_raw) {
                    can_compact = false;
                    break;
                }
            }
        }
    }

    bool modified = false;

    if (can_compact) {
        // compact resource data in ascending RVA order; each payload's bytes come from
        // a separate buffer (not from raw_data itself), so there's no read/write aliasing
        uint32_t write_cursor_rva = payloads.front().original_offset;
        for (auto& p : payloads) {
            // some resource types must stay 4-byte aligned to load correctly
            if ((p.original_offset & 3) == 0) {
                write_cursor_rva = round_up(write_cursor_rva, 4);
            }

            const uint32_t dest_raw = rsrc_raw_offset + (write_cursor_rva - rsrc_virtual_address);
            std::memcpy(raw_data.data() + dest_raw, p.data.data(), p.data.size());

            const auto new_size = static_cast<uint32_t>(p.data.size());
            std::memcpy(raw_data.data() + p.data_entry_offset, &write_cursor_rva, 4);
            std::memcpy(raw_data.data() + p.data_entry_offset + 4, &new_size, 4);

            write_cursor_rva += new_size;
        }

        const uint32_t new_data_end_rva = write_cursor_rva;
        const uint32_t new_virtual_size = new_data_end_rva - rsrc_virtual_address;
        const uint32_t new_data_end_raw = rsrc_raw_offset + new_virtual_size;
        const uint32_t new_data_end_raw_aligned = round_up(new_data_end_raw, layout.file_alignment);
        const uint32_t old_rsrc_end_raw = rsrc_raw_offset + rsrc_raw_size;

        // zero the small alignment gap left inside the (still full-size) section
        if (new_data_end_raw_aligned > new_data_end_raw) {
            std::memset(raw_data.data() + new_data_end_raw, 0, new_data_end_raw_aligned - new_data_end_raw);
        }

        // virtual-space savings (rounded to SectionAlignment), used for RVA fixups
        const uint32_t old_virtual_size_rounded = round_up(rsrc_dir_info.Size, layout.section_alignment);
        const uint32_t new_virtual_size_rounded = round_up(new_virtual_size, layout.section_alignment);
        const uint32_t virtual_saved = old_virtual_size_rounded > new_virtual_size_rounded
            ? old_virtual_size_rounded - new_virtual_size_rounded : 0;
        const uint32_t raw_saved = old_rsrc_end_raw - new_data_end_raw_aligned;

        // patch the section table: shift everything physically/virtually after .rsrc
        for (auto& sec : layout.sections) {
            if (sec.VirtualAddress > rsrc_virtual_address) {
                sec.VirtualAddress -= virtual_saved;
            } else if (sec.VirtualAddress == rsrc_virtual_address) {
                sec.SizeOfRawData = new_data_end_raw_aligned - rsrc_raw_offset;
                sec.VirtualSize = new_virtual_size;
            }
            if (sec.PointerToRawData > rsrc_raw_offset) {
                sec.PointerToRawData -= raw_saved;
            }
        }
        std::memcpy(raw_data.data() + layout.section_table_pos, layout.sections.data(),
                    layout.sections.size() * sizeof(ImageSectionHeader));

        // patch data directories whose target lies after the (now smaller) .rsrc section
        for (uint32_t i = 0; i < layout.num_data_dir; ++i) {
            const std::size_t dd_pos = layout.data_dir_offset + i * sizeof(ImageDataDirectory);
            ImageDataDirectory dd;
            std::memcpy(&dd, raw_data.data() + dd_pos, sizeof(ImageDataDirectory));
            if (dd.VirtualAddress >= rsrc_virtual_address + new_virtual_size_rounded) {
                dd.VirtualAddress -= virtual_saved;
                std::memcpy(raw_data.data() + dd_pos, &dd, sizeof(ImageDataDirectory));
            }
        }
        // shrink the resource directory entry's own declared size
        if (rsrc_dir_info.Size > new_virtual_size) {
            const std::size_t rsrc_dd_size_pos = layout.data_dir_offset + 2 * sizeof(ImageDataDirectory) + 4;
            std::memcpy(raw_data.data() + rsrc_dd_size_pos, &new_virtual_size, 4);
        }

        // SizeOfImage sits at the same fixed offset (56) in both PE32/PE32+
        const std::size_t size_of_image_pos = layout.pe_pos + 24 + 56;
        uint32_t size_of_image;
        std::memcpy(&size_of_image, raw_data.data() + size_of_image_pos, 4);
        size_of_image -= virtual_saved;
        std::memcpy(raw_data.data() + size_of_image_pos, &size_of_image, 4);

        // physically remove the now-unused trailing gap; std::vector::erase safely
        // shifts every subsequent byte (later sections, overlay/certificate data) back
        if (raw_saved > 0) {
            raw_data.erase(raw_data.begin() + new_data_end_raw_aligned, raw_data.begin() + old_rsrc_end_raw);
        }

        modified = true;
    } else {
        // non-standard/packed resource layout: fall back to the conservative
        // in-place overwrite (same total file size, but still applies whatever
        // optimizations were found -- safe for arbitrary/unusual layouts)
        for (const auto& p : payloads) {
            if (p.data.size() > p.original_size) continue; // never accept a larger result
            const uint32_t off = rva_to_offset(p.original_offset, layout.sections);
            if (off == 0 || static_cast<uint64_t>(off) + p.data.size() > raw_data.size()) continue;

            std::memcpy(raw_data.data() + off, p.data.data(), p.data.size());
            const auto new_size = static_cast<uint32_t>(p.data.size());
            std::memcpy(raw_data.data() + p.data_entry_offset + 4, &new_size, 4);
            if (new_size != p.original_size) modified = true;
        }
    }

    if (modified) {
        // invalidate Authenticode signature to prevent "corrupt file" OS warnings
        const uint32_t sec_dir_pos = layout.data_dir_offset + 4 * sizeof(ImageDataDirectory);
        if (static_cast<uint64_t>(sec_dir_pos) + 8 <= raw_data.size()) {
            uint32_t zero = 0;
            std::memcpy(raw_data.data() + sec_dir_pos, &zero, 4);     // VirtualAddress = 0
            std::memcpy(raw_data.data() + sec_dir_pos + 4, &zero, 4); // Size = 0
        }

        const uint32_t new_checksum = calculate_pe_checksum(raw_data, layout.pe_pos);
        const uint32_t checksum_offset = layout.pe_pos + 24 + 64;
        if (checksum_offset + 4 <= raw_data.size()) {
            std::memcpy(raw_data.data() + checksum_offset, &new_checksum, 4);
        }

        Logger::log(LogLevel::Debug, "patched pe checksum and invalidated authenticode", get_name());
    }

    std::filesystem::path output_path = std::filesystem::temp_directory_path() /
        (content.original_path.stem().string() + "_final" + RandomUtils::random_suffix() + content.original_path.extension().string());

    std::ofstream out_file(output_path, std::ios::binary);
    out_file.write(reinterpret_cast<const char*>(raw_data.data()), static_cast<std::streamsize>(raw_data.size()));
    out_file.close();

    cleanup_temp_dir(content.temp_dir, get_name());
    return output_path;
}

bool PeProcessor::raw_equal(const std::filesystem::path& a, const std::filesystem::path& b) const {
    return read_file(a) == read_file(b);
}

std::string PeProcessor::get_raw_checksum(const std::filesystem::path& /*file_path*/) const  {
    return "";
}

} // namespace chisel
