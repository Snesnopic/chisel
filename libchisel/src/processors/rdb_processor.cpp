//
// Created by Giuseppe Francione on 26/03/26.
//

#include "../../include/rdb_processor.hpp"
#include "../../include/logger.hpp"
#include "../../include/random_utils.hpp"
#include "../../include/file_utils.hpp"
#include <fstream>
#include <iomanip>
#include <sstream>

namespace chisel {


std::optional<ExtractedContent> RdbProcessor::prepare_extraction(const std::filesystem::path& input_path) {
    Logger::log(LogLevel::Debug, "starting rdb extraction for " + input_path.string(), get_name());

    const auto data = read_file(input_path);
    if (data.size() <= 0x20) return std::nullopt;

    const uint8_t magic[] = { 0x35, 0x33, 0x31, 0x45, 0x39, 0x38, 0x32, 0x30, 0x34, 0x46, 0x38, 0x35, 0x34, 0x32, 0x46, 0x30 };
    for (int i = 0; i < 16; i++) {
        if (data[i] != magic[i]) return std::nullopt;
    }

    uint32_t file_num = read_le32(data.data() + 0x10);
    uint64_t index_offset = read_le64(data.data() + 0x14);
    uint64_t content_offset = index_offset + read_le64(data.data() + 0x1C);

    if (content_offset < index_offset || content_offset > data.size()) {
        Logger::log(LogLevel::Warning, "rdb offsets out of bounds", get_name());
        return std::nullopt;
    }

    ExtractedContent content;
    content.original_path = input_path;
    content.temp_dir = make_temp_dir_for(input_path, "rdb");
    content.format = ContainerFormat::Unknown;

    size_t p_index = index_offset;

    for (uint32_t i = 0; i < file_num; i++) {
        // skip UTF-16 filename (search for 0x0000)
        while (p_index + 1 < content_offset && (data[p_index] != 0 || data[p_index + 1] != 0)) {
            p_index += 2;
        }
        p_index += 2; // skip the null terminator itself

        if (p_index + 16 > content_offset) break;

        uint64_t file_offset = read_le64(data.data() + p_index); // Absolute offset in file
        uint64_t file_size = read_le64(data.data() + p_index + 8);

        if (file_size > 0 && file_offset + file_size <= data.size()) {
            std::filesystem::path inner_path = content.temp_dir / (format_index(i) + ".bin");
            std::ofstream out_file(inner_path, std::ios::binary);
            out_file.write(reinterpret_cast<const char*>(data.data() + file_offset), file_size);
            out_file.close();

            content.extracted_files.push_back(inner_path);
        }

        p_index += 16;
    }

    return content;
}

std::filesystem::path RdbProcessor::finalize_extraction(const ExtractedContent& content, const ProcessingOptions&) {
    Logger::log(LogLevel::Debug, "starting rdb finalization for " + content.original_path.string(), get_name());

    const auto orig_data = read_file(content.original_path);
    uint32_t file_num = read_le32(orig_data.data() + 0x10);
    uint64_t index_offset = read_le64(orig_data.data() + 0x14);
    uint64_t content_offset = index_offset + read_le64(orig_data.data() + 0x1C);

    // copy header and index section
    std::vector<uint8_t> new_rdb;
    new_rdb.insert(new_rdb.end(), orig_data.begin(), orig_data.begin() + content_offset);

    size_t p_index = index_offset;
    uint64_t current_write_offset = content_offset;
    size_t extracted_idx = 0;

    for (uint32_t i = 0; i < file_num; i++) {
        while (p_index + 1 < content_offset && (new_rdb[p_index] != 0 || new_rdb[p_index + 1] != 0)) {
            p_index += 2;
        }
        p_index += 2;

        if (p_index + 16 > content_offset) break;

        uint64_t orig_size = read_le64(new_rdb.data() + p_index + 8);

        if (orig_size > 0) {
            const auto opt_payload = read_file(content.extracted_files[extracted_idx++]);

            // update offset and size in the index
            write_le64(new_rdb.data() + p_index, current_write_offset);
            write_le64(new_rdb.data() + p_index + 8, static_cast<uint64_t>(opt_payload.size()));

            // append payload
            new_rdb.insert(new_rdb.end(), opt_payload.begin(), opt_payload.end());
            current_write_offset += opt_payload.size();
        }

        p_index += 16;
    }

    std::filesystem::path output_path = std::filesystem::temp_directory_path() /
        (content.original_path.stem().string() + "_final" + RandomUtils::random_suffix() + ".rdb");

    std::ofstream out_file(output_path, std::ios::binary);
    out_file.write(reinterpret_cast<const char*>(new_rdb.data()), new_rdb.size());
    out_file.close();

    cleanup_temp_dir(content.temp_dir, get_name());
    return output_path;
}

bool RdbProcessor::raw_equal(const std::filesystem::path& a, const std::filesystem::path& b) const {
    const auto data_a = read_file(a);
    const auto data_b = read_file(b);
    return data_a == data_b;
}

} // namespace chisel
