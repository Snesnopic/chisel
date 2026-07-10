//
// Created by Giuseppe Francione on 25/03/26.
//

#include "../../include/zstd_processor.hpp"
#include "../../include/logger.hpp"
#include "../../include/random_utils.hpp"
#include "../../include/file_utils.hpp"
#include <zstd.h>
#include <fstream>
#include <vector>
#include <stdexcept>


namespace chisel {

// extract pure payload from a zstd stream, decoding all concatenated frames
static std::vector<uint8_t> decode_zstd(const std::vector<uint8_t>& compressed) {
    ZSTD_DStream* dstream = ZSTD_createDStream();
    if (!dstream) throw std::runtime_error("ZSTD_createDStream FAILED");

    size_t const init_result = ZSTD_initDStream(dstream);
    if (ZSTD_isError(init_result)) {
        ZSTD_freeDStream(dstream);
        throw std::runtime_error(std::string("ZSTD_initDStream FAILED: ") + ZSTD_getErrorName(init_result));
    }

    std::vector<uint8_t> decompressed;
    std::vector<uint8_t> out_buf(ZSTD_DStreamOutSize());
    ZSTD_inBuffer in = { compressed.data(), compressed.size(), 0 };

    while (in.pos < in.size) {
        ZSTD_outBuffer out = { out_buf.data(), out_buf.size(), 0 };
        size_t const result = ZSTD_decompressStream(dstream, &out, &in);
        if (ZSTD_isError(result)) {
            ZSTD_freeDStream(dstream);
            throw std::runtime_error(std::string("ZSTD DECOMPRESSION FAILED: ") + ZSTD_getErrorName(result));
        }
        decompressed.insert(decompressed.end(), out_buf.data(), out_buf.data() + out.pos);
    }

    ZSTD_freeDStream(dstream);
    return decompressed;
}

std::optional<ExtractedContent> ZstdProcessor::prepare_extraction(const std::filesystem::path& input_path) {
    Logger::log(LogLevel::Debug, "STARTING ZSTD EXTRACTION FOR " + input_path.string(), get_name());

    ExtractedContent content;
    content.original_path = input_path;
    content.temp_dir = make_temp_dir_for(input_path, "zstd");

    const auto input_data = read_file(input_path);
    const auto raw_data = decode_zstd(input_data);

    // strip extension to expose inner format
    std::string inner_name = input_path.filename().string();
    if (inner_name.ends_with(".zst")) {
        inner_name = inner_name.substr(0, inner_name.size() - 4);
    } else {
        inner_name += ".dec";
    }

    std::filesystem::path inner_path = content.temp_dir / inner_name;

    std::ofstream out_file(inner_path, std::ios::binary);
    if (!out_file) throw std::runtime_error("CANNOT CREATE INNER FILE FOR ZSTD");

    out_file.write(reinterpret_cast<const char*>(raw_data.data()), raw_data.size());
    out_file.close();

    content.extracted_files.push_back(inner_path);
    content.format = ContainerFormat::Unknown;

    return content;
}

std::filesystem::path ZstdProcessor::finalize_extraction(const ExtractedContent& content, const ProcessingOptions&) {
    Logger::log(LogLevel::Debug, "STARTING ZSTD FINALIZATION FOR " + content.original_path.string(), get_name());

    if (content.extracted_files.empty()) {
        throw std::runtime_error("NO FILES TO COMPRESS FOR ZSTD");
    }

    const auto& inner_path = content.extracted_files.front();
    const auto raw_data = read_file(inner_path);

    size_t const max_out_size = ZSTD_compressBound(raw_data.size());
    std::vector<uint8_t> compressed(max_out_size);

    // force extreme compression level 22
    size_t const result = ZSTD_compress(compressed.data(), max_out_size, raw_data.data(), raw_data.size(), 22);

    if (ZSTD_isError(result)) {
        throw std::runtime_error(std::string("ZSTD COMPRESSION FAILED: ") + ZSTD_getErrorName(result));
    }
    compressed.resize(result);

    std::filesystem::path output_path = std::filesystem::temp_directory_path() /
        (content.original_path.stem().string() + "_final" + RandomUtils::random_suffix() + ".zst");

    std::ofstream out_file(output_path, std::ios::binary);
    if (!out_file) throw std::runtime_error("CANNOT OPEN OUTPUT FILE: " + output_path.string());

    out_file.write(reinterpret_cast<const char*>(compressed.data()), compressed.size());
    out_file.close();

    if (out_file.fail()) {
        throw std::runtime_error("FAILED TO WRITE ZSTD OUTPUT DATA");
    }

    cleanup_temp_dir(content.temp_dir, get_name());
    Logger::log(LogLevel::Debug, "FINISHED ZSTD FINALIZATION", get_name());

    return output_path;
}

bool ZstdProcessor::raw_equal(const std::filesystem::path& a, const std::filesystem::path& b) const {
    try {
        const auto data_a = read_file(a);
        const auto data_b = read_file(b);

        const auto raw_a = decode_zstd(data_a);
        const auto raw_b = decode_zstd(data_b);

        return raw_a == raw_b;
    } catch (...) {
        Logger::log(LogLevel::Error, "RAW_EQUAL COMPARISON FAILED DUE TO DECODING ERROR", get_name());
        return false;
    }
}

} // namespace chisel