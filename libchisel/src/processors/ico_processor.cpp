//
// Created by Giuseppe Francione on 26/03/26.
//

#include "../../include/ico_processor.hpp"
#include "../../include/logger.hpp"
#include "../../include/random_utils.hpp"
#include "../../include/file_utils.hpp"
#include <fstream>
#include <vector>
#include <stdexcept>
#include <iomanip>
#include <sstream>
#include <algorithm>

namespace chisel {

// little-endian helpers
inline uint16_t read_le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t read_le32(const uint8_t* p) { return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24)); }
inline void write_le16(uint8_t* p, uint16_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }
inline void write_le32(uint8_t* p, uint32_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}

// format zero-padded index for sorting
static std::string format_index(size_t index) {
    std::ostringstream oss;
    oss << std::setw(4) << std::setfill('0') << index;
    return oss.str();
}

std::optional<ExtractedContent> IcoProcessor::prepare_extraction(const std::filesystem::path& input_path) {
    Logger::log(LogLevel::Debug, "STARTING ICO EXTRACTION FOR " + input_path.string(), get_name());

    ExtractedContent content;
    content.original_path = input_path;
    content.temp_dir = make_temp_dir_for(input_path, "ico");

    const auto data = read_file(input_path);
    if (data.size() < 6) throw std::runtime_error("FILE TOO SMALL TO BE ICO/CUR");

    const uint16_t reserved = read_le16(data.data());
    const uint16_t type = read_le16(data.data() + 2);
    const uint16_t count = read_le16(data.data() + 4);

    if (reserved != 0 || (type != 1 && type != 2)) {
        throw std::runtime_error("INVALID ICO/CUR HEADER");
    }
    if (data.size() < 6 + static_cast<size_t>(count) * 16) {
        throw std::runtime_error("TRUNCATED ICO/CUR DIRECTORY");
    }

    const uint8_t png_magic[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};

    for (uint16_t i = 0; i < count; ++i) {
        const size_t dir_offset = 6 + i * 16;
        const uint32_t bytes_in_res = read_le32(data.data() + dir_offset + 8);
        const uint32_t image_offset = read_le32(data.data() + dir_offset + 12);

        if (image_offset + bytes_in_res > data.size() || bytes_in_res == 0) {
            Logger::log(LogLevel::Warning, "SKIPPING CORRUPT ICO ENTRY " + std::to_string(i), get_name());
            continue;
        }

        const uint8_t* payload = data.data() + image_offset;

        // detect payload type to assign correct extension for mime detector
        // detect payload type
        bool is_png = (bytes_in_res > 8 && std::memcmp(payload, png_magic, 8) == 0);
        std::string ext = is_png ? ".png" : ".bmp"; // usiamo bmp e non dib!

        std::filesystem::path out_path = content.temp_dir / (format_index(i) + ext);
        std::ofstream out_file(out_path, std::ios::binary);
        if (!out_file) throw std::runtime_error("CANNOT CREATE FILE: " + out_path.string());

        if (is_png) {
            out_file.write(reinterpret_cast<const char*>(payload), bytes_in_res);
        } else {
            uint32_t biSize = read_le32(payload);
            uint16_t biBitCount = read_le16(payload + 14);
            uint32_t biClrUsed = read_le32(payload + 32);

            uint32_t palette_colors = 0;
            if (biBitCount <= 8) {
                palette_colors = (biClrUsed == 0) ? (1 << biBitCount) : biClrUsed;
            }

            uint32_t pixel_offset = 14 + biSize + (palette_colors * 4);
            uint32_t file_size = 14 + bytes_in_res;

            uint8_t bmp_header[14] = {0};
            bmp_header[0] = 'B';
            bmp_header[1] = 'M';
            write_le32(bmp_header + 2, file_size);
            write_le32(bmp_header + 10, pixel_offset);

            out_file.write(reinterpret_cast<const char*>(bmp_header), 14);
            out_file.write(reinterpret_cast<const char*>(payload), bytes_in_res);
        }
        out_file.close();

        content.extracted_files.push_back(out_path);
    }

    content.format = ContainerFormat::Unknown;
    return content;
}

std::filesystem::path IcoProcessor::finalize_extraction(const ExtractedContent& content, const ProcessingOptions&) {
    Logger::log(LogLevel::Debug, "STARTING ICO FINALIZATION FOR " + content.original_path.string(), get_name());

    const auto orig_data = read_file(content.original_path);
    if (orig_data.size() < 6) throw std::runtime_error("ORIGINAL ICO CORRUPTED");

    const uint16_t count = read_le16(orig_data.data() + 4);

    // gather optimized files
    std::vector<std::filesystem::path> optimized_files;
    for (const auto& p : std::filesystem::directory_iterator(content.temp_dir)) {
        if (p.is_regular_file()) optimized_files.push_back(p.path());
    }

    // ensure order matches original index
    std::sort(optimized_files.begin(), optimized_files.end());

    if (optimized_files.size() != count) {
        throw std::runtime_error("MISMATCH BETWEEN EXTRACTED FILES AND ICO DIRECTORY");
    }

    std::vector<uint8_t> new_ico;
    new_ico.reserve(orig_data.size()); // max possible size

    // copy 6-byte header
    new_ico.insert(new_ico.end(), orig_data.data(), orig_data.data() + 6);

    // allocate space for new directory entries (16 bytes each)
    const size_t dir_start = new_ico.size();
    new_ico.resize(new_ico.size() + count * 16);

    uint32_t current_offset = static_cast<uint32_t>(new_ico.size());

    for (uint16_t i = 0; i < count; ++i) {
        auto payload = read_file(optimized_files[i]);
        if (payload.size() > 14 && payload[0] == 'B' && payload[1] == 'M') {
            payload.erase(payload.begin(), payload.begin() + 14);
        }
        // copy original 16-byte entry to preserve width/height/planes/bpp metadata
        const size_t orig_dir_offset = 6 + i * 16;
        uint8_t entry[16];
        memcpy(entry, orig_data.data() + orig_dir_offset, 16);

        // update size and offset
        write_le32(entry + 8, static_cast<uint32_t>(payload.size()));
        write_le32(entry + 12, current_offset);

        // write updated entry back into new_ico directory space
        memcpy(new_ico.data() + dir_start + i * 16, entry, 16);

        // append payload
        new_ico.insert(new_ico.end(), payload.begin(), payload.end());
        current_offset += static_cast<uint32_t>(payload.size());
    }

    std::filesystem::path output_path = std::filesystem::temp_directory_path() /
        (content.original_path.stem().string() + "_final" + RandomUtils::random_suffix() + content.original_path.extension().string());

    std::ofstream out_file(output_path, std::ios::binary);
    if (!out_file) throw std::runtime_error("CANNOT OPEN OUTPUT FILE: " + output_path.string());

    out_file.write(reinterpret_cast<const char*>(new_ico.data()), new_ico.size());
    out_file.close();

    if (out_file.fail()) {
        throw std::runtime_error("FAILED TO WRITE ICO OUTPUT DATA");
    }

    cleanup_temp_dir(content.temp_dir, get_name());
    Logger::log(LogLevel::Debug, "FINISHED ICO FINALIZATION", get_name());

    return output_path;
}

bool IcoProcessor::raw_equal(const std::filesystem::path& a, const std::filesystem::path& b) const {
    try {
        const auto data_a = read_file(a);
        const auto data_b = read_file(b);
        return data_a == data_b;
    } catch (...) {
        return false;
    }
}

} // namespace chisel