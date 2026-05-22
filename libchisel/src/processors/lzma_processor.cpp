//
// Created by Giuseppe Francione on 24/03/26.
//

#include "../../include/lzma_processor.hpp"
#include "../../include/logger.hpp"
#include "../../include/random_utils.hpp"
#include "../../include/file_utils.hpp"
#include "lzma.h"
#include <fstream>
#include <vector>
#include <stdexcept>


namespace chisel {

static std::vector<uint8_t> decode_lzma(const std::vector<uint8_t>& compressed) {
    lzma_stream strm = LZMA_STREAM_INIT;
    // auto_decoder handles both .xz and legacy .lzma
    lzma_ret ret = lzma_auto_decoder(&strm, UINT64_MAX, 0);
    if (ret != LZMA_OK) throw std::runtime_error("failed to init lzma decoder");

    std::vector<uint8_t> decompressed;
    const size_t chunk_size = 65536;
    std::vector<uint8_t> out_buf(chunk_size);

    strm.next_in = compressed.data();
    strm.avail_in = compressed.size();

    do {
        strm.next_out = out_buf.data();
        strm.avail_out = out_buf.size();

        ret = lzma_code(&strm, LZMA_FINISH);
        if (ret != LZMA_OK && ret != LZMA_STREAM_END) {
            lzma_end(&strm);
            throw std::runtime_error("lzma decompression failed");
        }

        size_t written = out_buf.size() - strm.avail_out;
        if (written > 0) {
            decompressed.insert(decompressed.end(), out_buf.data(), out_buf.data() + written);
        }
    } while (strm.avail_out == 0);

    lzma_end(&strm);
    return decompressed;
}

std::optional<ExtractedContent> LzmaProcessor::prepare_extraction(const std::filesystem::path& input_path) {
    Logger::log(LogLevel::Debug, "starting lzma extraction for " + input_path.string(), get_name());

    ExtractedContent content;
    content.original_path = input_path;
    content.temp_dir = make_temp_dir_for(input_path, "lzma");

    const auto input_data = read_file(input_path);
    const auto raw_data = decode_lzma(input_data);

    // strip extension to expose inner format
    std::string inner_name = input_path.filename().string();
    if (inner_name.ends_with(".xz")) {
        inner_name = inner_name.substr(0, inner_name.size() - 3);
    } else if (inner_name.ends_with(".lzma")) {
        inner_name = inner_name.substr(0, inner_name.size() - 5);
    } else {
        inner_name += ".dec";
    }

    std::filesystem::path inner_path = content.temp_dir / inner_name;

    std::ofstream out_file(inner_path, std::ios::binary);
    if (!out_file) throw std::runtime_error("cannot create inner file for lzma");

    out_file.write(reinterpret_cast<const char*>(raw_data.data()), raw_data.size());
    out_file.close();

    content.extracted_files.push_back(inner_path);
    content.format = ContainerFormat::Unknown;

    return content;
}

std::filesystem::path LzmaProcessor::finalize_extraction(const ExtractedContent& content, const ProcessingOptions&) {
    Logger::log(LogLevel::Debug, "starting lzma finalization for " + content.original_path.string(), get_name());

    if (content.extracted_files.empty()) {
        throw std::runtime_error("no files to compress for lzma");
    }

    const auto& inner_path = content.extracted_files.front();
    const auto raw_data = read_file(inner_path);

    lzma_stream strm = LZMA_STREAM_INIT;
    lzma_ret ret;

    // use appropriate encoder based on original extension
    bool is_legacy = content.original_path.extension().string() == ".lzma";
    if (is_legacy) {
        lzma_options_lzma opt;
        lzma_lzma_preset(&opt, 9 | LZMA_PRESET_EXTREME);
        ret = lzma_alone_encoder(&strm, &opt);
    } else {
        ret = lzma_easy_encoder(&strm, 9 | LZMA_PRESET_EXTREME, LZMA_CHECK_CRC64);
    }

    if (ret != LZMA_OK) throw std::runtime_error("failed to init lzma encoder");

    std::vector<uint8_t> compressed;
    const size_t chunk_size = 65536;
    std::vector<uint8_t> out_buf(chunk_size);

    strm.next_in = raw_data.data();
    strm.avail_in = raw_data.size();

    do {
        strm.next_out = out_buf.data();
        strm.avail_out = out_buf.size();

        ret = lzma_code(&strm, LZMA_FINISH);
        if (ret != LZMA_OK && ret != LZMA_STREAM_END) {
            lzma_end(&strm);
            throw std::runtime_error("lzma compression failed");
        }

        size_t written = out_buf.size() - strm.avail_out;
        if (written > 0) {
            compressed.insert(compressed.end(), out_buf.data(), out_buf.data() + written);
        }
    } while (strm.avail_out == 0);

    lzma_end(&strm);

    std::filesystem::path output_path = std::filesystem::temp_directory_path() /
        (content.original_path.stem().string() + "_final" + RandomUtils::random_suffix() + content.original_path.extension().string());

    std::ofstream out_file(output_path, std::ios::binary);
    if (!out_file) throw std::runtime_error("cannot open output file: " + output_path.string());

    out_file.write(reinterpret_cast<const char*>(compressed.data()), compressed.size());
    out_file.close();

    if (out_file.fail()) {
        throw std::runtime_error("failed to write lzma output data");
    }

    cleanup_temp_dir(content.temp_dir);
    Logger::log(LogLevel::Debug, "finished lzma finalization", get_name());

    return output_path;
}

bool LzmaProcessor::raw_equal(const std::filesystem::path& a, const std::filesystem::path& b) const {
    try {
        const auto data_a = read_file(a);
        const auto data_b = read_file(b);

        const auto raw_a = decode_lzma(data_a);
        const auto raw_b = decode_lzma(data_b);

        return raw_a == raw_b;
    } catch (...) {
        Logger::log(LogLevel::Error, "raw_equal comparison failed due to decoding error", get_name());
        return false;
    }
}

} // namespace chisel
