//
// Created by Giuseppe Francione on 24/09/26.
//

#include "../../include/data_uri_processor.hpp"
#include "../../include/data_uri.hpp"
#include "../../include/file_type.hpp"
#include "../../include/file_utils.hpp"
#include "../../include/logger.hpp"
#include "../../include/random_utils.hpp"
#include <string_view>
#include <vector>

namespace chisel {

namespace fs = std::filesystem;

std::optional<ExtractedContent> DataUriProcessor::prepare_extraction(const std::filesystem::path& input_path) {
    Logger::log(LogLevel::Debug, "Entering prepare_extraction for " + input_path.filename().string(), get_name());

    ExtractedContent content;
    content.original_path = input_path;
    content.format = ContainerFormat::Unknown;
    std::vector<DataUriAsset> assets;
    if (std::vector<uint8_t> bytes; read_file(input_path, bytes)) {
        const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        assets = extract_data_uris(text, [&] {
            content.temp_dir = make_temp_dir_for(input_path, "datauri");
            return content.temp_dir;
        });
    }
    for (const auto& asset : assets) {
        content.extracted_files.push_back(asset.file);
    }
    Logger::log(LogLevel::Debug, "Extracted " + std::to_string(assets.size()) + " data URIs", get_name());
    content.extras = std::move(assets);

    Logger::log(LogLevel::Debug, "Exiting prepare_extraction for " + input_path.filename().string(), get_name());
    return content;
}

std::filesystem::path DataUriProcessor::finalize_extraction(const ExtractedContent& content, const ProcessingOptions&) {
    Logger::log(LogLevel::Debug, "Entering finalize_extraction for " + content.original_path.filename().string(), get_name());

    std::optional<std::string> rebuilt;
    const auto* assets = std::any_cast<std::vector<DataUriAsset>>(&content.extras);
    if (std::vector<uint8_t> bytes; assets != nullptr && !assets->empty() && read_file(content.original_path, bytes)) {
        rebuilt = reinsert_data_uris(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), *assets);
    }
    if (!content.temp_dir.empty()) {
        cleanup_temp_dir(content.temp_dir, get_name());
    }
    if (!rebuilt) {
        Logger::log(LogLevel::Debug, "No data URI got smaller", get_name());
        return {};
    }

    const fs::path out_path = fs::temp_directory_path() /
                              (content.original_path.stem().string() + "_tmp" + RandomUtils::random_suffix() +
                               content.original_path.extension().string());
    if (!write_file(out_path, std::vector<uint8_t>(rebuilt->begin(), rebuilt->end()))) {
        Logger::log(LogLevel::Error, "Can't write " + out_path.string(), get_name());
        std::error_code ec;
        fs::remove(out_path, ec);
        return {};
    }

    Logger::log(LogLevel::Debug, "Exiting finalize_extraction for " + out_path.filename().string(), get_name());
    return out_path;
}

std::string DataUriProcessor::get_raw_checksum(const std::filesystem::path& /*file_path*/) const {
    return "";
}

} // namespace chisel
