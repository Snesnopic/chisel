//
// Created by Giuseppe Francione on 23/03/26.
//

#include "../../include/woff2_processor.hpp"
#include "../../include/logger.hpp"
#include <woff2/decode.h>
#include <woff2/encode.h>
#include <fstream>
#include <vector>
#include <stdexcept>


namespace chisel {

// decode woff2 container to raw ttf payload
static std::vector<uint8_t> decode_to_ttf(const std::vector<uint8_t>& woff2_data) {
    const size_t ttf_size = woff2::ComputeWOFF2FinalSize(woff2_data.data(), woff2_data.size());
    if (ttf_size == 0) {
        throw std::runtime_error("Invalid WOFF2 container");
    }
    std::vector<uint8_t> ttf_data(ttf_size);
    if (!woff2::ConvertWOFF2ToTTF(ttf_data.data(), ttf_size, woff2_data.data(), woff2_data.size())) {
        throw std::runtime_error("Decoding WOFF2 to TTF failed");
    }

    ttf_data.resize(ttf_size);
    return ttf_data;
}

void Woff2Processor::recompress(const std::filesystem::path& input,
                                const std::filesystem::path& output,
                                const ProcessingOptions& /*options*/) {
    Logger::log(LogLevel::Debug, "Starting WOFF2 recompression for " + input.string(), get_name());

    const auto input_data = chisel::read_file(input);
    const auto ttf_data = decode_to_ttf(input_data);

    size_t max_woff2_size = woff2::MaxWOFF2CompressedSize(ttf_data.data(), ttf_data.size());
    std::vector<uint8_t> output_data(max_woff2_size);

    // force highest brotli dictionary quality
    woff2::WOFF2Params params;
    params.brotli_quality = 11;

    if (!woff2::ConvertTTFToWOFF2(ttf_data.data(), ttf_data.size(),
                                  output_data.data(), &max_woff2_size, params)) {
        throw std::runtime_error("Encoding TTF to WOFF2 failed");
    }
    output_data.resize(max_woff2_size);

    std::ofstream out_file(output, std::ios::binary);
    if (!out_file) throw std::runtime_error("Cannot open output file: " + output.string());
    out_file.write(reinterpret_cast<const char*>(output_data.data()), output_data.size());

    Logger::log(LogLevel::Debug, "Finished WOFF2 recompression for " + output.string(), get_name());
}

std::string Woff2Processor::get_raw_checksum(const std::filesystem::path& /*file_path*/) const {
    return "";
}

bool Woff2Processor::raw_equal(const std::filesystem::path& a, const std::filesystem::path& b) const {
    try {
        const auto data_a = chisel::read_file(a);
        const auto data_b = chisel::read_file(b);

        const auto ttf_a = decode_to_ttf(data_a);
        const auto ttf_b = decode_to_ttf(data_b);

        return ttf_a == ttf_b;
    } catch (...) {
        Logger::log(LogLevel::Error, "raw_equal comparison failed due to decoding error", get_name());
        return false;
    }
}

} // namespace chisel