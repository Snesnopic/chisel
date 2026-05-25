//
// Created by Giuseppe Francione on 25/03/26.
//

#include "../../include/bzip2_processor.hpp"
#include "../../include/logger.hpp"
#include "../../include/random_utils.hpp"
#include "../../include/file_utils.hpp"
#include <bzlib.h>
#include <fstream>
#include <vector>
#include <stdexcept>


namespace chisel {

// extract pure payload from bzip2 stream
static std::vector<uint8_t> decode_bzip2(const std::vector<uint8_t>& compressed) {
    bz_stream strm{};
    int ret = BZ2_bzDecompressInit(&strm, 0, 0);
    if (ret != BZ_OK) throw std::runtime_error("FAILED TO INIT BZIP2 DECODER");

    std::vector<uint8_t> decompressed;
    const size_t chunk_size = 65536;
    std::vector<uint8_t> out_buf(chunk_size);

    // bzlib expects char* instead of uint8_t*
    strm.next_in = reinterpret_cast<char*>(const_cast<uint8_t*>(compressed.data()));
    strm.avail_in = static_cast<unsigned int>(compressed.size());

    do {
        strm.next_out = reinterpret_cast<char*>(out_buf.data());
        strm.avail_out = static_cast<unsigned int>(out_buf.size());

        ret = BZ2_bzDecompress(&strm);
        if (ret != BZ_OK && ret != BZ_STREAM_END) {
            BZ2_bzDecompressEnd(&strm);
            throw std::runtime_error("BZIP2 DECOMPRESSION FAILED");
        }

        size_t written = out_buf.size() - strm.avail_out;
        if (written > 0) {
            decompressed.insert(decompressed.end(), out_buf.data(), out_buf.data() + written);
        }
    } while (ret != BZ_STREAM_END);

    BZ2_bzDecompressEnd(&strm);
    return decompressed;
}

std::optional<ExtractedContent> Bzip2Processor::prepare_extraction(const std::filesystem::path& input_path) {
    Logger::log(LogLevel::Debug, "STARTING BZIP2 EXTRACTION FOR " + input_path.string(), get_name());

    ExtractedContent content;
    content.original_path = input_path;
    content.temp_dir = make_temp_dir_for(input_path, "bzip2");

    const auto input_data = read_file(input_path);
    const auto raw_data = decode_bzip2(input_data);

    // strip extension to expose inner format
    std::string inner_name = input_path.filename().string();
    if (inner_name.ends_with(".bz2")) {
        inner_name = inner_name.substr(0, inner_name.size() - 4);
    } else {
        inner_name += ".dec";
    }

    std::filesystem::path inner_path = content.temp_dir / inner_name;

    std::ofstream out_file(inner_path, std::ios::binary);
    if (!out_file) throw std::runtime_error("CANNOT CREATE INNER FILE FOR BZIP2");

    out_file.write(reinterpret_cast<const char*>(raw_data.data()), raw_data.size());
    out_file.close();

    content.extracted_files.push_back(inner_path);
    content.format = ContainerFormat::Unknown;

    return content;
}

std::filesystem::path Bzip2Processor::finalize_extraction(const ExtractedContent& content, const ProcessingOptions&) {
    Logger::log(LogLevel::Debug, "STARTING BZIP2 FINALIZATION FOR " + content.original_path.string(), get_name());

    if (content.extracted_files.empty()) {
        throw std::runtime_error("NO FILES TO COMPRESS FOR BZIP2");
    }

    const auto& inner_path = content.extracted_files.front();
    const auto raw_data = read_file(inner_path);

    bz_stream strm{};
    // blocksize100k = 9 (max compression), verbosity = 0, workfactor = 0 (default)
    int ret = BZ2_bzCompressInit(&strm, 9, 0, 0);
    if (ret != BZ_OK) throw std::runtime_error("FAILED TO INIT BZIP2 ENCODER");

    std::vector<uint8_t> compressed;
    const size_t chunk_size = 65536;
    std::vector<uint8_t> out_buf(chunk_size);

    strm.next_in = reinterpret_cast<char*>(const_cast<uint8_t*>(raw_data.data()));
    strm.avail_in = static_cast<unsigned int>(raw_data.size());

    do {
        strm.next_out = reinterpret_cast<char*>(out_buf.data());
        strm.avail_out = static_cast<unsigned int>(out_buf.size());

        ret = BZ2_bzCompress(&strm, BZ_FINISH);
        if (ret != BZ_FINISH_OK && ret != BZ_STREAM_END) {
            BZ2_bzCompressEnd(&strm);
            throw std::runtime_error("BZIP2 COMPRESSION FAILED");
        }

        size_t written = out_buf.size() - strm.avail_out;
        if (written > 0) {
            compressed.insert(compressed.end(), out_buf.data(), out_buf.data() + written);
        }
    } while (ret != BZ_STREAM_END);

    BZ2_bzCompressEnd(&strm);

    std::filesystem::path output_path = std::filesystem::temp_directory_path() /
        (content.original_path.stem().string() + "_final" + RandomUtils::random_suffix() + ".bz2");

    std::ofstream out_file(output_path, std::ios::binary);
    if (!out_file) throw std::runtime_error("CANNOT OPEN OUTPUT FILE: " + output_path.string());

    out_file.write(reinterpret_cast<const char*>(compressed.data()), compressed.size());
    out_file.close();

    if (out_file.fail()) {
        throw std::runtime_error("FAILED TO WRITE BZIP2 OUTPUT DATA");
    }

    cleanup_temp_dir(content.temp_dir, get_name());
    Logger::log(LogLevel::Debug, "FINISHED BZIP2 FINALIZATION", get_name());

    return output_path;
}

bool Bzip2Processor::raw_equal(const std::filesystem::path& a, const std::filesystem::path& b) const {
    try {
        const auto data_a = read_file(a);
        const auto data_b = read_file(b);

        const auto raw_a = decode_bzip2(data_a);
        const auto raw_b = decode_bzip2(data_b);

        return raw_a == raw_b;
    } catch (...) {
        Logger::log(LogLevel::Error, "RAW_EQUAL COMPARISON FAILED DUE TO DECODING ERROR", get_name());
        return false;
    }
}

} // namespace chisel