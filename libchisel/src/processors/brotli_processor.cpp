//
// Created by Giuseppe Francione on 24/03/26.
//

#include "../../include/brotli_processor.hpp"
#include "../../include/logger.hpp"
#include "../../include/random_utils.hpp"
#include "file_utils.hpp"
#include <brotli/decode.h>
#include <brotli/encode.h>
#include <fstream>
#include <vector>
#include <stdexcept>


namespace chisel {

static std::vector<uint8_t> decode_brotli(const std::vector<uint8_t>& compressed) {
    BrotliDecoderState* state = BrotliDecoderCreateInstance(nullptr, nullptr, nullptr);
    if (!state) throw std::runtime_error("Failed to create brotli decoder");

    std::vector<uint8_t> decompressed;
    size_t available_in = compressed.size();
    const uint8_t* next_in = compressed.data();

    const size_t chunk_size = 65536;
    std::vector<uint8_t> chunk(chunk_size);

    BrotliDecoderResult result;
    do {
        size_t available_out = chunk_size;
        uint8_t* next_out = chunk.data();

        result = BrotliDecoderDecompressStream(
            state, &available_in, &next_in, &available_out, &next_out, nullptr);

        if (result == BROTLI_DECODER_RESULT_ERROR) {
            BrotliDecoderDestroyInstance(state);
            throw std::runtime_error("Brotli decompression failed");
        }

        size_t written = chunk_size - available_out;
        if (written > 0) {
            decompressed.insert(decompressed.end(), chunk.data(), chunk.data() + written);
        }
    } while (result == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT);

    BrotliDecoderDestroyInstance(state);
    return decompressed;
}

std::optional<ExtractedContent> BrotliProcessor::prepare_extraction(const std::filesystem::path& input_path) {
    Logger::log(LogLevel::Debug, "Starting brotli extraction for " + input_path.string(), get_name());

    ExtractedContent content;
    content.original_path = input_path;
    content.temp_dir = make_temp_dir_for(input_path, "brotli");

    const auto input_data = read_file(input_path);
    const auto raw_data = decode_brotli(input_data);

    // strip .br to expose inner extension to mime detector
    std::string inner_name = input_path.filename().string();
    if (inner_name.ends_with(".br")) {
        inner_name = inner_name.substr(0, inner_name.size() - 3);
    } else {
        inner_name += ".dec";
    }

    std::filesystem::path inner_path = content.temp_dir / inner_name;

    std::ofstream out_file(inner_path, std::ios::binary);
    if (!out_file) {
        throw std::runtime_error("Can't create inner file for brotli");
    }
    out_file.write(reinterpret_cast<const char*>(raw_data.data()), raw_data.size());
    out_file.close();

    content.extracted_files.push_back(inner_path);
    content.format = ContainerFormat::Unknown;

    return content;
}

std::filesystem::path BrotliProcessor::finalize_extraction(const ExtractedContent& content, const ProcessingOptions& /*options*/) {
    Logger::log(LogLevel::Debug, "Starting brotli finalization for " + content.original_path.string(), get_name());

    if (content.extracted_files.empty()) {
        throw std::runtime_error("No files to compress for brotli");
    }

    // single file stream
    const auto& inner_path = content.extracted_files.front();
    const auto raw_data = read_file(inner_path);

    const size_t max_out_size = BrotliEncoderMaxCompressedSize(raw_data.size());
    std::vector<uint8_t> output_data(max_out_size);
    size_t out_size = max_out_size;

    const BROTLI_BOOL ok = BrotliEncoderCompress(
        11,
        24,
        BROTLI_DEFAULT_MODE,
        raw_data.size(),
        raw_data.data(),
        &out_size,
        output_data.data()
    );

    if (!ok) {
        throw std::runtime_error("Brotli compression failed");
    }
    output_data.resize(out_size);

    std::filesystem::path output_path = std::filesystem::temp_directory_path() /
        (content.original_path.stem().string() + "_final" + RandomUtils::random_suffix() + ".br");

    std::ofstream out_file(output_path, std::ios::binary);
    if (!out_file) throw std::runtime_error("Can't open output file: " + output_path.string());

    out_file.write(reinterpret_cast<const char*>(output_data.data()), output_data.size());
    out_file.close();

    if (out_file.fail()) {
        throw std::runtime_error("Failed to write brotli output data");
    }

    cleanup_temp_dir(content.temp_dir);

    Logger::log(LogLevel::Debug, "Finished brotli finalization for " + output_path.string(), get_name());
    return output_path;
}

bool BrotliProcessor::raw_equal(const std::filesystem::path& a, const std::filesystem::path& b) const {
    try {
        const auto data_a = read_file(a);
        const auto data_b = read_file(b);

        const auto raw_a = decode_brotli(data_a);
        const auto raw_b = decode_brotli(data_b);

        return raw_a == raw_b;
    } catch (...) {
        Logger::log(LogLevel::Error, "raw_equal comparison failed due to decoding error", get_name());
        return false;
    }
}

} // namespace chisel