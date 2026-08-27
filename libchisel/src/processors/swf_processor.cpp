//
// Created by Giuseppe Francione on 26/03/26.
//

#include "../../include/swf_processor.hpp"
#include "../../include/logger.hpp"
#include "../../include/file_utils.hpp"
#include "../../include/zopfli_compressor.hpp"
#include "file_utils.hpp"
#include <zlib.h>
#include <fstream>
#include <vector>
#include <stdexcept>


namespace chisel {

namespace {

// matches pdf_processor's own threshold for switching to the (usually lower) large-payload iteration count
constexpr size_t kLargeSwfPayloadThreshold = 200000;

unsigned pick_iterations(const size_t data_size, const ProcessingOptions& options) {
    return static_cast<unsigned>(data_size < kLargeSwfPayloadThreshold ? options.iterations : options.iterations_large);
}

} // namespace

// inflate zlib payload
static std::vector<uint8_t> inflate_swf(const uint8_t* src, const std::size_t src_len, const std::size_t expected_len) {
    std::vector<uint8_t> uncompressed(expected_len);
    uLongf dest_len = static_cast<uLongf>(expected_len);

    if (uncompress(uncompressed.data(), &dest_len, src, static_cast<uLong>(src_len)) != Z_OK) {
        throw std::runtime_error("zlib decompression failed");
    }

    uncompressed.resize(dest_len);
    return uncompressed;
}

void SwfProcessor::recompress(const std::filesystem::path& input_path,
                              const std::filesystem::path& output_path, const ProcessingOptions& options) {
    Logger::log(LogLevel::Debug, "starting swf recompression for " + input_path.string(), get_name());

    const auto data = read_file(input_path);
    if (data.size() < 8) return;

    const uint8_t magic[3] = {data[0], data[1], data[2]};
    const uint8_t version = data[3];
    const uint32_t uncompressed_file_size = read_le32(data.data() + 4);

    if (magic[1] != 'W' || magic[2] != 'S') {
        Logger::log(LogLevel::Warning, "invalid swf signature", get_name());
        return;
    }

    std::vector<uint8_t> uncompressed_payload;

    try {
        if (magic[0] == 'F') {
            uncompressed_payload.assign(data.begin() + 8, data.end());
        } else if (magic[0] == 'C') {
            const std::size_t expected_size = uncompressed_file_size > 8 ? uncompressed_file_size - 8 : 0;
            uncompressed_payload = inflate_swf(data.data() + 8, data.size() - 8, expected_size);
        } else if (magic[0] == 'Z') {
            // lzma swf (ZWS) is rare and requires special setup. skip for now to maintain safety.
            Logger::log(LogLevel::Warning, "lzma swf (ZWS) not supported, skipping", get_name());
            return;
        } else {
            return;
        }

        const auto compressed_payload = ZopfliCompressor::compress(
            uncompressed_payload, pick_iterations(uncompressed_payload.size(), options), ZopfliFormat::ZLIB);

        std::ofstream out(output_path, std::ios::binary);
        if (!out) {
            Logger::log(LogLevel::Error, "cannot open output file: " + output_path.string(), get_name());
            return;
        }

        // force standard zlib compression header (CWS)
        out.write("CWS", 3);
        out.write(reinterpret_cast<const char*>(&version), 1);

        uint8_t size_bytes[4];
        write_le32(size_bytes, static_cast<uint32_t>(uncompressed_payload.size() + 8));
        out.write(reinterpret_cast<const char*>(size_bytes), 4);

        out.write(reinterpret_cast<const char*>(compressed_payload.data()), compressed_payload.size());
        out.close();

    } catch (...) {
        Logger::log(LogLevel::Error, "failed to recompress swf payload", get_name());
    }
}

// verify bit-identical logic (uncompressed payloads must match)
bool SwfProcessor::raw_equal(const std::filesystem::path& a, const std::filesystem::path& b) const {
    try {
        auto get_uncompressed = [](const std::vector<uint8_t>& data) -> std::vector<uint8_t> {
            if (data.size() < 8) return {};
            if (data[0] == 'F') return {data.begin() + 8, data.end()};
            if (data[0] == 'C') {
                const uint32_t declared_size = read_le32(data.data() + 4);
                const std::size_t expected = declared_size > 8 ? declared_size - 8 : 0;
                return inflate_swf(data.data() + 8, data.size() - 8, expected);
            }
            return {};
        };

        const auto data_a = read_file(a);
        const auto data_b = read_file(b);

        if (data_a.size() < 8 || data_b.size() < 8) return false;
        if (data_a[3] != data_b[3]) return false; // versions must match

        return get_uncompressed(data_a) == get_uncompressed(data_b);
    } catch (...) {
        return false;
    }
}

} // namespace chisel