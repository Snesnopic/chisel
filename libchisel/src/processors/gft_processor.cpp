//
// Created by Giuseppe Francione on 26/03/26.
//

#include "../../include/gft_processor.hpp"
#include "../../include/logger.hpp"
#include "../../include/random_utils.hpp"
#include "../../include/file_utils.hpp"
#include <fstream>


namespace chisel {

std::optional<ExtractedContent> GftProcessor::prepare_extraction(const std::filesystem::path& input_path) {
    Logger::log(LogLevel::Debug, "starting gft extraction for " + input_path.string(), get_name());

    const auto data = read_file(input_path);

    // min header size + magic check
    if (data.size() < 0x14) return std::nullopt;

    const uint8_t magic[] = { 0x54, 0x47, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00 };
    for (int i = 0; i < 8; i++) {
        if (data[i] != magic[i]) return std::nullopt;
    }

    uint32_t header_size = read_le32(data.data() + 0x10);
    if (data.size() <= header_size) {
        Logger::log(LogLevel::Warning, "gft file is truncated or empty", get_name());
        return std::nullopt;
    }

    ExtractedContent content;
    content.original_path = input_path;
    content.temp_dir = make_temp_dir_for(input_path, "gft");
    content.format = ContainerFormat::Unknown;

    const uint8_t* payload = data.data() + header_size;
    std::size_t payload_size = data.size() - header_size;

    // guess extension to help the pipeline
    std::string ext = ".bin";
    if (payload_size > 2 && payload[0] == 0x89 && payload[1] == 'P') ext = ".png";
    else if (payload_size > 2 && payload[0] == 0xFF && payload[1] == 0xD8) ext = ".jpg";
    else if (payload_size > 3 && payload[0] == 'G' && payload[1] == 'I' && payload[2] == 'F') ext = ".gif";

    std::filesystem::path inner_path = content.temp_dir / ("inner_image" + ext);
    std::ofstream out_file(inner_path, std::ios::binary);
    out_file.write(reinterpret_cast<const char*>(payload), payload_size);
    out_file.close();

    content.extracted_files.push_back(inner_path);
    content.extras = std::make_any<std::size_t>(header_size); // save for later

    return content;
}

std::filesystem::path GftProcessor::finalize_extraction(const ExtractedContent& content, const ProcessingOptions&) {
    Logger::log(LogLevel::Debug, "starting gft finalization for " + content.original_path.string(), get_name());

    const auto orig_data = read_file(content.original_path);
    const auto header_size = std::any_cast<std::size_t>(content.extras);
    const auto opt_payload = read_file(content.extracted_files.front());

    std::filesystem::path output_path = std::filesystem::temp_directory_path() /
        (content.original_path.stem().string() + "_final" + RandomUtils::random_suffix() + ".gft");

    std::ofstream out_file(output_path, std::ios::binary);
    if (!out_file) throw std::runtime_error("cannot open output gft file");

    out_file.write(reinterpret_cast<const char*>(orig_data.data()), header_size);
    out_file.write(reinterpret_cast<const char*>(opt_payload.data()), opt_payload.size());
    out_file.close();

    cleanup_temp_dir(content.temp_dir, get_name());
    return output_path;
}

} // namespace chisel
