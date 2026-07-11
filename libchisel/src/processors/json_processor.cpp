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

    // parse JSON; BIGNUM_AS_RAW preserves the exact original text for numbers
    // that don't fit in int64_t/uint64_t/finite double instead of silently
    // rounding them through a double on the way to re-serialization
    yyjson_read_err err;
    yyjson_doc *doc = yyjson_read_opts(buffer.data(), buffer.size(), YYJSON_READ_BIGNUM_AS_RAW, nullptr, &err);
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
    yyjson_doc *doc_a = yyjson_read_file(a.string().c_str(), YYJSON_READ_BIGNUM_AS_RAW, nullptr, nullptr);
    yyjson_doc *doc_b = yyjson_read_file(b.string().c_str(), YYJSON_READ_BIGNUM_AS_RAW, nullptr, nullptr);

    if (!doc_a || !doc_b) {
        if (doc_a) yyjson_doc_free(doc_a);
        if (doc_b) yyjson_doc_free(doc_b);
        return false;
    }

    size_t len_a = 0, len_b = 0;
    char *json_a = yyjson_write(doc_a, 0, &len_a);
    char *json_b = yyjson_write(doc_b, 0, &len_b);

    bool equal = false;
    if (json_a && json_b && len_a == len_b) {
        equal = (memcmp(json_a, json_b, len_a) == 0);
    }

    if (json_a) free(json_a);
    if (json_b) free(json_b);
    yyjson_doc_free(doc_a);
    yyjson_doc_free(doc_b);

    return equal;
}

} // namespace chisel
