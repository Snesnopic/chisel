//
// Created by Giuseppe Francione on 25/03/26.
//

#include "../../include/lz4_processor.hpp"
#include "../../include/logger.hpp"
#include "../../include/random_utils.hpp"
#include "../../include/file_utils.hpp"
#include <lz4frame.h>
#include <fstream>
#include <vector>
#include <stdexcept>

namespace chisel {

static std::vector<uint8_t> decode_lz4(const std::vector<uint8_t>& compressed) {
    LZ4F_dctx* dctx;
    const LZ4F_errorCode_t err = LZ4F_createDecompressionContext(&dctx, LZ4F_VERSION);
    if (LZ4F_isError(err)) {
        throw std::runtime_error("CANNOT CREATE LZ4 DECOMPRESSION CONTEXT");
    }

    std::vector<uint8_t> decompressed;
    const std::size_t srcSize = compressed.size();
    std::size_t srcPtr = 0;
    
    // Decompress chunk by chunk since output size is unknown
    constexpr std::size_t dstCapacity = 64 * 1024; // 64 KB initial chunk
    std::vector<uint8_t> dstBuf(dstCapacity);

    while (srcPtr < srcSize) {
        std::size_t srcRemaining = srcSize - srcPtr;
        std::size_t dstSize = dstCapacity;
        
        const std::size_t ret = LZ4F_decompress(dctx, dstBuf.data(), &dstSize, compressed.data() + srcPtr, &srcRemaining, nullptr);
        
        if (LZ4F_isError(ret)) {
            LZ4F_freeDecompressionContext(dctx);
            throw std::runtime_error(std::string("LZ4 DECOMPRESSION FAILED: ") + LZ4F_getErrorName(ret));
        }
        
        if (dstSize > 0) {
            decompressed.insert(decompressed.end(), dstBuf.begin(), dstBuf.begin() + dstSize);
        }
        srcPtr += srcRemaining;
        
        if (ret == 0) break; // Frame fully decoded
    }
    
    LZ4F_freeDecompressionContext(dctx);
    return decompressed;
}

std::optional<ExtractedContent> Lz4Processor::prepare_extraction(const std::filesystem::path& input_path) {
    Logger::log(LogLevel::Debug, "STARTING LZ4 EXTRACTION FOR " + input_path.string(), get_name());

    ExtractedContent content;
    content.original_path = input_path;
    content.temp_dir = make_temp_dir_for(input_path, "lz4");

    const auto input_data = read_file(input_path);
    const auto raw_data = decode_lz4(input_data);

    // strip extension to expose inner format
    std::string inner_name = input_path.filename().string();
    if (inner_name.ends_with(".lz4")) {
        inner_name = inner_name.substr(0, inner_name.size() - 4);
    } else {
        inner_name += ".dec";
    }

    std::filesystem::path inner_path = content.temp_dir / inner_name;

    std::ofstream out_file(inner_path, std::ios::binary);
    if (!out_file) throw std::runtime_error("CANNOT CREATE INNER FILE FOR LZ4");

    out_file.write(reinterpret_cast<const char*>(raw_data.data()), raw_data.size());
    out_file.close();

    content.extracted_files.push_back(inner_path);
    content.format = ContainerFormat::Unknown;

    return content;
}

std::filesystem::path Lz4Processor::finalize_extraction(const ExtractedContent& content, const ProcessingOptions& options) {
    Logger::log(LogLevel::Debug, "STARTING LZ4 FINALIZATION FOR " + content.original_path.string(), get_name());

    if (content.extracted_files.empty()) {
        throw std::runtime_error("NO FILES TO COMPRESS FOR LZ4");
    }

    const auto& inner_path = content.extracted_files.front();
    const auto raw_data = read_file(inner_path);

    LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
    prefs.compressionLevel = 12; // LZ4HC maximum level
    if (!options.preserve_metadata) {
        prefs.frameInfo.contentChecksumFlag = LZ4F_noContentChecksum;
        prefs.frameInfo.blockChecksumFlag = LZ4F_noBlockChecksum;
    }

    std::size_t const bound = LZ4F_compressFrameBound(raw_data.size(), &prefs);
    std::vector<uint8_t> compressed(bound);

    std::size_t const result = LZ4F_compressFrame(compressed.data(), bound, raw_data.data(), raw_data.size(), &prefs);

    if (LZ4F_isError(result)) {
        throw std::runtime_error(std::string("LZ4 COMPRESSION FAILED: ") + LZ4F_getErrorName(result));
    }
    compressed.resize(result);

    std::filesystem::path output_path = std::filesystem::temp_directory_path() /
        (content.original_path.stem().string() + "_final" + RandomUtils::random_suffix() + ".lz4");

    std::ofstream out_file(output_path, std::ios::binary);
    if (!out_file) throw std::runtime_error("CANNOT OPEN OUTPUT FILE: " + output_path.string());

    out_file.write(reinterpret_cast<const char*>(compressed.data()), compressed.size());
    out_file.close();

    if (out_file.fail()) {
        throw std::runtime_error("FAILED TO WRITE LZ4 OUTPUT DATA");
    }

    cleanup_temp_dir(content.temp_dir, get_name());
    Logger::log(LogLevel::Debug, "FINISHED LZ4 FINALIZATION", get_name());

    return output_path;
}

bool Lz4Processor::raw_equal(const std::filesystem::path& a, const std::filesystem::path& b) const {
    try {
        const auto data_a = read_file(a);
        const auto data_b = read_file(b);

        const auto raw_a = decode_lz4(data_a);
        const auto raw_b = decode_lz4(data_b);

        return raw_a == raw_b;
    } catch (...) {
        Logger::log(LogLevel::Error, "RAW_EQUAL COMPARISON FAILED DUE TO DECODING ERROR", get_name());
        return false;
    }
}

} // namespace chisel
