//
// Created by Giuseppe Francione on 26/03/26.
//

#include "../../include/icns_processor.hpp"
#include "../../include/logger.hpp"
#include "../../include/random_utils.hpp"
#include "../../include/file_utils.hpp"
#include <fstream>
#include <vector>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <cstring>


namespace chisel {

std::optional<ExtractedContent> IcnsProcessor::prepare_extraction(const std::filesystem::path& input_path) {
    Logger::log(LogLevel::Debug, "STARTING ICNS EXTRACTION FOR " + input_path.string(), get_name());

    const auto data = read_file(input_path);
    if (data.size() < 8) return std::nullopt;

    if (data[0] != 'i' || data[1] != 'c' || data[2] != 'n' || data[3] != 's') {
        Logger::log(LogLevel::Warning, "INVALID ICNS MAGIC NUMBER", get_name());
        return std::nullopt;
    }

    uint32_t file_size = read_be32(data.data() + 4);
    if (file_size > data.size()) {
        Logger::log(LogLevel::Warning, "TRUNCATED ICNS FILE", get_name());
        return std::nullopt;
    }

    ExtractedContent content;
    content.original_path = input_path;
    content.temp_dir = make_temp_dir_for(input_path, "icns");
    content.format = ContainerFormat::Unknown;

    std::vector<uint32_t> ostypes;
    size_t offset = 8;
    int index = 0;

    const uint8_t png_magic[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    const uint8_t jp2_magic[] = {0x00, 0x00, 0x00, 0x0C, 0x6A, 0x50, 0x20, 0x20, 0x0D, 0x0A, 0x87, 0x0A};

    while (offset + 8 <= file_size && offset + 8 <= data.size()) {
        uint32_t ostype = read_be32(data.data() + offset);
        uint32_t block_size = read_be32(data.data() + offset + 4);

        if (block_size < 8 || offset + block_size > data.size()) break;

        size_t payload_size = block_size - 8;
        const uint8_t* payload = data.data() + offset + 8;

        std::string ext = ".bin";
        if (payload_size >= 8 && std::memcmp(payload, png_magic, 8) == 0) {
            ext = ".png";
        } else if (payload_size >= 12 && std::memcmp(payload, jp2_magic, 12) == 0) {
            ext = ".jp2";
        }

        std::filesystem::path out_path = content.temp_dir / (format_index(index) + ext);
        std::ofstream out_file(out_path, std::ios::binary);
        out_file.write(reinterpret_cast<const char*>(payload), payload_size);
        out_file.close();

        content.extracted_files.push_back(out_path);
        ostypes.push_back(ostype);

        offset += block_size;
        index++;
    }

    content.extras = std::make_any<std::vector<uint32_t>>(ostypes);
    return content;
}

std::filesystem::path IcnsProcessor::finalize_extraction(const ExtractedContent& content, const ProcessingOptions&) {
    Logger::log(LogLevel::Debug, "STARTING ICNS FINALIZATION FOR " + content.original_path.string(), get_name());

    auto ostypes = std::any_cast<std::vector<uint32_t>>(content.extras);

    std::vector<std::filesystem::path> optimized_files;
    for (const auto& p : std::filesystem::directory_iterator(content.temp_dir)) {
        if (p.is_regular_file()) optimized_files.push_back(p.path());
    }
    std::sort(optimized_files.begin(), optimized_files.end());

    if (optimized_files.size() != ostypes.size()) {
        throw std::runtime_error("MISMATCH BETWEEN EXTRACTED FILES AND ICNS BLOCKS");
    }

    std::vector<uint8_t> new_icns;
    // reserve space for header
    new_icns.resize(8);
    new_icns[0] = 'i'; new_icns[1] = 'c'; new_icns[2] = 'n'; new_icns[3] = 's';

    for (size_t i = 0; i < optimized_files.size(); ++i) {
        auto payload = read_file(optimized_files[i]);
        uint32_t block_size = static_cast<uint32_t>(payload.size() + 8);

        uint8_t block_header[8];
        write_be32(block_header, ostypes[i]);
        write_be32(block_header + 4, block_size);

        new_icns.insert(new_icns.end(), block_header, block_header + 8);
        new_icns.insert(new_icns.end(), payload.begin(), payload.end());
    }

    // update total size in the header
    write_be32(new_icns.data() + 4, static_cast<uint32_t>(new_icns.size()));

    std::filesystem::path output_path = std::filesystem::temp_directory_path() /
        (content.original_path.stem().string() + "_final" + RandomUtils::random_suffix() + ".icns");

    std::ofstream out_file(output_path, std::ios::binary);
    out_file.write(reinterpret_cast<const char*>(new_icns.data()), new_icns.size());
    out_file.close();

    cleanup_temp_dir(content.temp_dir, get_name());
    return output_path;
}

bool IcnsProcessor::raw_equal(const std::filesystem::path& a, const std::filesystem::path& b) const {
    try {
        const auto data_a = read_file(a);
        const auto data_b = read_file(b);
        return data_a == data_b;
    } catch (...) {
        return false;
    }
}

} // namespace chisel