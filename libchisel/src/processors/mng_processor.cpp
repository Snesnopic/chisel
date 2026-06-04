//
// Created by Giuseppe Francione on 04/06/26.
//

#include "../../include/mng_processor.hpp"
#include "../../include/logger.hpp"
#include "../../include/file_utils.hpp"
#include "../../include/zopfli_compressor.hpp"
#include <jpeglib.h>
#include <zlib.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <stdexcept>
#include <iostream>
#include <cstring>
#include <memory>
#include <arpa/inet.h> // for ntohl/htonl

namespace chisel {

namespace {

struct Chunk {
    uint32_t length;
    char type[5];
    std::vector<uint8_t> data;
    uint32_t crc;
};

uint32_t read_u32(std::istream& is) {
    uint32_t val;
    is.read(reinterpret_cast<char*>(&val), 4);
    return ntohl(val);
}

void write_u32(std::ostream& os, uint32_t val) {
    uint32_t nval = htonl(val);
    os.write(reinterpret_cast<const char*>(&nval), 4);
}

void update_crc(Chunk& chunk) {
    uint32_t crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, reinterpret_cast<const uint8_t*>(chunk.type), 4);
    if (!chunk.data.empty()) {
        crc = crc32(crc, chunk.data.data(), chunk.data.size());
    }
    chunk.crc = crc;
}

// simplified mozjpeg optimization (like in JpegProcessor but from memory buffer)
std::vector<uint8_t> optimize_jpeg(const std::vector<uint8_t>& input) {
    struct jpeg_decompress_struct srcinfo;
    struct jpeg_compress_struct dstinfo;
    struct jpeg_error_mgr jsrcerr, jdsterr;

    srcinfo.err = jpeg_std_error(&jsrcerr);
    dstinfo.err = jpeg_std_error(&jdsterr);

    jpeg_create_decompress(&srcinfo);
    jpeg_create_compress(&dstinfo);

    jpeg_mem_src(&srcinfo, input.data(), input.size());
    if (jpeg_read_header(&srcinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_compress(&dstinfo);
        jpeg_destroy_decompress(&srcinfo);
        return input; // fallback to original
    }

    jvirt_barray_ptr *coef_arrays = jpeg_read_coefficients(&srcinfo);
    jpeg_copy_critical_parameters(&srcinfo, &dstinfo);
    dstinfo.optimize_coding = TRUE;

    uint8_t *out_buf = nullptr;
    unsigned long out_size = 0;
    jpeg_mem_dest(&dstinfo, &out_buf, &out_size);

    jpeg_write_coefficients(&dstinfo, coef_arrays);
    jpeg_finish_compress(&dstinfo);
    jpeg_finish_decompress(&srcinfo);

    std::vector<uint8_t> result(out_buf, out_buf + out_size);

    jpeg_destroy_compress(&dstinfo);
    jpeg_destroy_decompress(&srcinfo);
    // Note: out_buf is managed by libjpeg/mozjpeg (or free if using newer API)
    // For safety with mozjpeg's jpeg_mem_dest, we might need a custom manager if it doesn't use malloc
    if (out_buf) free(out_buf);

    return result;
}

std::vector<uint8_t> decompress_deflate(const std::vector<uint8_t>& input) {
    std::vector<uint8_t> decompressed;
    z_stream strm;
    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;
    strm.avail_in = static_cast<uint32_t>(input.size());
    strm.next_in = const_cast<uint8_t*>(input.data());

    if (inflateInit(&strm) != Z_OK) return {};

    uint8_t buffer[32768];
    do {
        strm.avail_out = sizeof(buffer);
        strm.next_out = buffer;
        int ret = inflate(&strm, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END) {
            inflateEnd(&strm);
            return {};
        }
        decompressed.insert(decompressed.end(), buffer, buffer + (sizeof(buffer) - strm.avail_out));
    } while (strm.avail_out == 0);
    inflateEnd(&strm);
    return decompressed;
}

static std::vector<uint8_t> optimize_deflate(const std::vector<uint8_t>& input, int iterations) {
    auto raw = decompress_deflate(input);
    if (raw.empty()) return input;

    auto result = ZopfliCompressor::compress(raw, static_cast<unsigned>(iterations), ZopfliFormat::ZLIB);
    return (result.size() < input.size()) ? result : input;
}

} // namespace

void MngProcessor::recompress(const std::filesystem::path& input,
                               const std::filesystem::path& output,
                               const ProcessingOptions &options) {
    Logger::log(LogLevel::Debug, "Entering recompress for " + input.string(), get_name());

    std::ifstream is(input, std::ios::binary);
    if (!is) throw std::runtime_error("MngProcessor: cannot open input");

    // check signature
    uint8_t sig[8];
    is.read(reinterpret_cast<char*>(sig), 8);
    bool is_mng = (std::memcmp(sig, "\x8A\x4D\x4E\x47\x0D\x0A\x1A\x0A", 8) == 0);
    bool is_jng = (std::memcmp(sig, "\x8B\x4A\x4E\x47\x0D\x0A\x1A\x0A", 8) == 0);

    if (!is_mng && !is_jng) {
        throw std::runtime_error("MngProcessor: invalid MNG/JNG signature");
    }

    std::ofstream os(output, std::ios::binary);
    os.write(reinterpret_cast<const char*>(sig), 8);

    while (is.peek() != EOF) {
        Chunk chunk;
        chunk.length = read_u32(is);
        is.read(chunk.type, 4);
        chunk.type[4] = '\0';
        chunk.data.resize(chunk.length);
        if (chunk.length > 0) {
            is.read(reinterpret_cast<char*>(chunk.data.data()), chunk.length);
        }
        chunk.crc = read_u32(is);

        // --- OPTIMIZATION LOGIC ---
        bool modified = false;
        std::string type(chunk.type);

        if (is_mng && type == "IDAT") {
            // Re-compress MNG image data
            chunk.data = optimize_deflate(chunk.data, options.iterations);
            modified = true;
        } else if (is_jng && type == "JDAT") {
            // Re-compress JNG JPEG data
            chunk.data = optimize_jpeg(chunk.data);
            modified = true;
        } else if (is_jng && type == "IDAT") {
            // Re-compress JNG Alpha channel (Deflate)
            chunk.data = optimize_deflate(chunk.data, options.iterations);
            modified = true;
        }

        if (modified) {
            chunk.length = static_cast<uint32_t>(chunk.data.size());
            update_crc(chunk);
        }

        // write chunk
        write_u32(os, chunk.length);
        os.write(chunk.type, 4);
        if (chunk.length > 0) {
            os.write(reinterpret_cast<const char*>(chunk.data.data()), chunk.length);
        }
        write_u32(os, chunk.crc);

        if (type == "MEND" || type == "IEND") break;
    }

    Logger::log(LogLevel::Debug, "Exiting recompress for " + output.string(), get_name());
}

static std::vector<uint8_t> decode_jpeg_to_pixels(const std::vector<uint8_t>& input) {
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, input.data(), input.size());
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) { jpeg_destroy_decompress(&cinfo); return {}; }
    jpeg_start_decompress(&cinfo);
    int w = cinfo.output_width;
    int h = cinfo.output_height;
    int c = cinfo.output_components;
    std::vector<uint8_t> buffer(static_cast<size_t>(w) * h * c);
    while (cinfo.output_scanline < h) {
        uint8_t* row_ptr = buffer.data() + (cinfo.output_scanline * w * c);
        jpeg_read_scanlines(&cinfo, &row_ptr, 1);
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return buffer;
}

std::string MngProcessor::get_raw_checksum(const std::filesystem::path& /*file_path*/) const {
    return "";
}

bool MngProcessor::raw_equal(const std::filesystem::path& a, const std::filesystem::path& b) const {
    auto get_payloads = [](const std::filesystem::path& p) {
        std::vector<std::vector<uint8_t>> payloads;
        std::ifstream is(p, std::ios::binary);
        if (!is) return payloads;
        uint8_t sig[8]; is.read(reinterpret_cast<char*>(sig), 8);
        bool is_jng = (sig[0] == 0x8B);
        while (is.peek() != EOF) {
            uint32_t len = read_u32(is);
            char type[4]; is.read(type, 4);
            std::vector<uint8_t> data(len);
            if (len > 0) is.read(reinterpret_cast<char*>(data.data()), len);
            read_u32(is); // crc
            std::string stype(type, 4);
            if (stype == "IDAT") payloads.push_back(decompress_deflate(data));
            else if (is_jng && stype == "JDAT") payloads.push_back(decode_jpeg_to_pixels(data));
            if (stype == "MEND" || stype == "IEND") break;
        }
        return payloads;
    };

    auto payloadsA = get_payloads(a);
    auto payloadsB = get_payloads(b);
    return payloadsA == payloadsB;
}

} // namespace chisel
