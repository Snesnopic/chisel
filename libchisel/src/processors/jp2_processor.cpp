//
// Created by Giuseppe Francione on 04/06/26.
//

#include "../../include/jp2_processor.hpp"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <optional>
#include <vector>
#include "../../include/logger.hpp"
#include <openjpeg.h>
#include <filesystem>
#include <stdexcept>
#include <iostream>

namespace chisel {

// openjpeg logging callbacks
static void error_callback(const char* msg, void* /*client_data*/) {
    Logger::log(LogLevel::Error, "OpenJPEG Error: " + std::string(msg), "Jp2Processor");
}

static void warning_callback(const char* msg, void* /*client_data*/) {
    Logger::log(LogLevel::Warning, "OpenJPEG Warning: " + std::string(msg), "Jp2Processor");
}

static void info_callback(const char* msg, void* /*client_data*/) {
    Logger::log(LogLevel::Debug, "OpenJPEG Info: " + std::string(msg), "Jp2Processor");
}

// content-based detection (extension-based broke on the executor's .tmp-renamed pipeline files)
static OPJ_CODEC_FORMAT detect_codec_format(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    uint8_t buf[12] = {0};
    f.read(reinterpret_cast<char*>(buf), sizeof(buf));

    static constexpr uint8_t jp2_rfc3745_magic[12] = {
        0x00, 0x00, 0x00, 0x0c, 0x6a, 0x50, 0x20, 0x20, 0x0d, 0x0a, 0x87, 0x0a};
    static constexpr uint8_t jp2_magic[4] = {0x0d, 0x0a, 0x87, 0x0a};
    static constexpr uint8_t j2k_magic[4] = {0xff, 0x4f, 0xff, 0x51};

    if (std::memcmp(buf, jp2_rfc3745_magic, 12) == 0 || std::memcmp(buf, jp2_magic, 4) == 0) {
        return OPJ_CODEC_JP2;
    }
    if (std::memcmp(buf, j2k_magic, 4) == 0) {
        return OPJ_CODEC_J2K;
    }
    return OPJ_CODEC_JP2; // fallback, matches the previous default
}

// opj_j2k_read_com is a no-op stub in openjpeg, so the COM marker text is pulled from raw bytes here instead
static std::optional<std::string> extract_com_comment(const std::filesystem::path& path) {
    std::vector<uint8_t> data;
    {
        std::ifstream f(path, std::ios::binary);
        if (!f) return std::nullopt;
        data.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    if (data.size() < 4) return std::nullopt;

    auto read_be16 = [](const uint8_t* p) -> uint16_t {
        return static_cast<uint16_t>((static_cast<uint32_t>(p[0]) << 8) | p[1]);
    };
    auto read_be32 = [](const uint8_t* p) -> uint32_t {
        return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 8) | p[3];
    };

    const uint8_t* codestream = nullptr;
    size_t codestream_len = 0;

    if (data.size() >= 2 && data[0] == 0xFF && data[1] == 0x4F) {
        // raw .j2k/.j2c: the file itself is the codestream, starting at SOC
        codestream = data.data();
        codestream_len = data.size();
    } else {
        // .jp2: box-based container, find the 'jp2c' box holding the codestream
        size_t pos = 0;
        while (pos + 8 <= data.size()) {
            uint64_t box_len = read_be32(data.data() + pos);
            const uint8_t* type = data.data() + pos + 4;
            size_t header_size = 8;

            if (box_len == 1) {
                if (pos + 16 > data.size()) break;
                box_len = (static_cast<uint64_t>(read_be32(data.data() + pos + 8)) << 32) |
                          read_be32(data.data() + pos + 12);
                header_size = 16;
            } else if (box_len == 0) {
                box_len = data.size() - pos;
            }
            if (box_len < header_size || pos + box_len > data.size()) break;

            if (std::memcmp(type, "jp2c", 4) == 0) {
                codestream = data.data() + pos + header_size;
                codestream_len = box_len - header_size;
                break;
            }
            pos += box_len;
        }
    }

    if (!codestream || codestream_len < 4) return std::nullopt;

    // walk main-header markers for COM (0xFF64), stop at SOT/SOD/EOC
    size_t p = 2; // skip SOC
    while (p + 4 <= codestream_len) {
        if (codestream[p] != 0xFF) break;
        const uint8_t marker = codestream[p + 1];
        if (marker == 0x93 /*SOD*/ || marker == 0x90 /*SOT*/ || marker == 0xD9 /*EOC*/) break;

        const uint16_t seg_len = read_be16(codestream + p + 2); // includes itself, excludes marker code
        if (seg_len < 2 || p + 2 + static_cast<size_t>(seg_len) > codestream_len) break;

        if (marker == 0x64 /*COM*/) {
            const size_t payload_len = seg_len - 2;
            if (payload_len < 2) break;
            const uint8_t* payload = codestream + p + 4;
            const uint16_t rcom = read_be16(payload);
            const size_t text_len = payload_len - 2;
            if (rcom == 1) { // iso-8859-1 text only; binary comments can't round-trip as a c-string
                return std::string(reinterpret_cast<const char*>(payload + 2), text_len);
            }
            return std::nullopt;
        }

        p += 2 + seg_len;
    }

    return std::nullopt;
}

void Jp2Processor::recompress(const std::filesystem::path& input_path,
                               const std::filesystem::path& output_path,
                               const ProcessingOptions &/*options*/) {
    Logger::log(LogLevel::Debug, "Entering recompress for " + input_path.string(), get_name());

    const OPJ_CODEC_FORMAT format = detect_codec_format(input_path);

    // --- DECODER SETUP ---
    opj_dparameters_t dparam;
    opj_set_default_decoder_parameters(&dparam);

    opj_stream_t* in_stream = opj_stream_create_default_file_stream(input_path.string().c_str(), OPJ_TRUE);
    if (!in_stream) {
        throw std::runtime_error("Jp2Processor: failed to open input file stream");
    }

    opj_codec_t* decoder = opj_create_decompress(format);
    opj_set_info_handler(decoder, info_callback, nullptr);
    opj_set_warning_handler(decoder, warning_callback, nullptr);
    opj_set_error_handler(decoder, error_callback, nullptr);

    if (!opj_setup_decoder(decoder, &dparam)) {
        opj_stream_destroy(in_stream);
        opj_destroy_codec(decoder);
        throw std::runtime_error("Jp2Processor: failed to setup decoder");
    }

    opj_image_t* image = nullptr;
    if (!opj_read_header(in_stream, decoder, &image)) {
        opj_stream_destroy(in_stream);
        opj_destroy_codec(decoder);
        throw std::runtime_error("Jp2Processor: failed to read header");
    }

    if (!opj_decode(decoder, in_stream, image)) {
        opj_image_destroy(image);
        opj_stream_destroy(in_stream);
        opj_destroy_codec(decoder);
        throw std::runtime_error("Jp2Processor: failed to decode image");
    }
    
    opj_end_decompress(decoder, in_stream);
    opj_stream_destroy(in_stream);
    opj_destroy_codec(decoder);

    // --- ENCODER SETUP ---
    opj_cparameters_t cparam;
    opj_set_default_encoder_parameters(&cparam);

    // lossless configuration
    cparam.tcp_numlayers = 1;
    cparam.tcp_rates[0] = 0;
    cparam.cp_disto_alloc = 1;
    cparam.irreversible = 0; // use 5/3 wavelet transform

    // opj_setup_encoder copies cp_comment internally, so it only needs to stay valid for this call
    auto comment = extract_com_comment(input_path);
    if (comment) {
        cparam.cp_comment = comment->data();
    }

    opj_codec_t* encoder = opj_create_compress(format);
    opj_set_info_handler(encoder, info_callback, nullptr);
    opj_set_warning_handler(encoder, warning_callback, nullptr);
    opj_set_error_handler(encoder, error_callback, nullptr);

    if (!opj_setup_encoder(encoder, &cparam, image)) {
        opj_image_destroy(image);
        opj_destroy_codec(encoder);
        throw std::runtime_error("Jp2Processor: failed to setup encoder");
    }

    opj_stream_t* out_stream = opj_stream_create_default_file_stream(output_path.string().c_str(), OPJ_FALSE);
    if (!out_stream) {
        opj_image_destroy(image);
        opj_destroy_codec(encoder);
        throw std::runtime_error("Jp2Processor: failed to open output file stream");
    }

    // execute compression
    bool success = opj_start_compress(encoder, image, out_stream) &&
                   opj_encode(encoder, out_stream) &&
                   opj_end_compress(encoder, out_stream);

    // cleanup
    opj_stream_destroy(out_stream);
    opj_destroy_codec(encoder);
    opj_image_destroy(image);

    if (!success) {
        throw std::runtime_error("Jp2Processor: compression pipeline failed");
    }

    Logger::log(LogLevel::Debug, "Exiting recompress for " + output_path.string(), get_name());
}

std::optional<ExtractedContent> Jp2Processor::prepare_extraction(const std::filesystem::path& /*input_path*/) {
    return std::nullopt;
}

std::filesystem::path Jp2Processor::finalize_extraction(const ExtractedContent& /*content*/, const ProcessingOptions &/*options*/) {
    return {};
}

static std::vector<uint8_t> decode_jp2_rgba(const std::filesystem::path& path, int& w, int& h) {
    const OPJ_CODEC_FORMAT format = detect_codec_format(path);

    opj_dparameters_t dparam;
    opj_set_default_decoder_parameters(&dparam);
    opj_stream_t* stream = opj_stream_create_default_file_stream(path.string().c_str(), OPJ_TRUE);
    if (!stream) return {};

    opj_codec_t* decoder = opj_create_decompress(format);
    if (!opj_setup_decoder(decoder, &dparam)) {
        opj_stream_destroy(stream);
        opj_destroy_codec(decoder);
        return {};
    }

    opj_image_t* image = nullptr;
    if (!opj_read_header(stream, decoder, &image) || !opj_decode(decoder, stream, image)) {
        if (image) opj_image_destroy(image);
        opj_stream_destroy(stream);
        opj_destroy_codec(decoder);
        return {};
    }

    w = static_cast<int>(image->x1 - image->x0);
    h = static_cast<int>(image->y1 - image->y0);
    const std::size_t size = static_cast<size_t>(w) * h * image->numcomps;
    std::vector<uint8_t> pixels(size * sizeof(int));

    for (uint32_t i = 0; i < image->numcomps; ++i) {
        if (image->comps[i].data) {
            std::memcpy(pixels.data() + (i * w * h * sizeof(int)), image->comps[i].data, static_cast<size_t>(w) * h * sizeof(int));
        }
    }

    opj_image_destroy(image);
    opj_stream_destroy(stream);
    opj_destroy_codec(decoder);
    return pixels;
}

std::string Jp2Processor::get_raw_checksum(const std::filesystem::path& /*file_path*/) const {
    return "";
}

bool Jp2Processor::raw_equal(const std::filesystem::path& a, const std::filesystem::path& b) const {
    int wa, ha, wb, hb;
    const auto pixA = decode_jp2_rgba(a, wa, ha);
    const auto pixB = decode_jp2_rgba(b, wb, hb);

    if (pixA.empty() || pixB.empty()) return false;
    if (wa != wb || ha != hb) return false;
    return pixA == pixB;
}

} // namespace chisel
