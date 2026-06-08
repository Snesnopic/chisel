//
// Created by Giuseppe Francione on 04/06/26.
//

#include "../../include/json_processor.hpp"
#include "../../include/logger.hpp"
#include <yyjson.h>
#include <filesystem>
#include <fstream>
#include <vector>

namespace chisel {

void JsonProcessor::recompress(const std::filesystem::path& input_path,
                               const std::filesystem::path& output_path,
                               const ProcessingOptions &options) {
    Logger::log(LogLevel::Debug, "Entering recompress for " + input_path.string(), get_name());

    // read the input file
    std::ifstream ifs(input_path, std::ios::binary | std::ios::ate);
    if (!ifs) {
        throw std::runtime_error("JsonProcessor: could not open input file");
    }
    auto size = ifs.tellg();
    ifs.seekg(0, std::ios::beg);
    std::vector<char> buffer(static_cast<size_t>(size));
    if (!ifs.read(buffer.data(), size)) {
        throw std::runtime_error("JsonProcessor: could not read input file");
    }

    // parse JSON
    yyjson_read_err err;
    yyjson_doc *doc = yyjson_read_opts(buffer.data(), buffer.size(), 0, nullptr, &err);
    if (!doc) {
        Logger::log(LogLevel::Error, "JSON parse error: " + std::string(err.msg) + " at " + std::to_string(err.pos), get_name());
        throw std::runtime_error("JsonProcessor: failed to parse JSON");
    }

    // write minified JSON
    size_t out_len;
    char *json = yyjson_write(doc, 0, &out_len);
    if (!json) {
        yyjson_doc_free(doc);
        throw std::runtime_error("JsonProcessor: failed to write JSON");
    }

    // save to output path
    std::ofstream ofs(output_path, std::ios::binary);
    if (!ofs) {
        free(json);
        yyjson_doc_free(doc);
        throw std::runtime_error("JsonProcessor: could not open output file");
    }
    ofs.write(json, static_cast<std::streamsize>(out_len));

    // cleanup
    free(json);
    yyjson_doc_free(doc);

    Logger::log(LogLevel::Debug, "Exiting recompress for " + output_path.string(), get_name());
}

std::optional<ExtractedContent> JsonProcessor::prepare_extraction(const std::filesystem::path& /*input_path*/) {
    return std::nullopt;
}

std::filesystem::path JsonProcessor::finalize_extraction(const ExtractedContent& /*content*/, const ProcessingOptions &/*options*/) {
    return {};
}

std::string JsonProcessor::get_raw_checksum(const std::filesystem::path& /*file_path*/) const {
    return "";
}

bool JsonProcessor::raw_equal(const std::filesystem::path& a, const std::filesystem::path& b) const {
    // TODO: 
    return false;
}

} // namespace chisel
