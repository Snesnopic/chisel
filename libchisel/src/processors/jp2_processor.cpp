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
#include "../../include/file_utils.hpp"
#include <openjpeg.h>
#include <iterator>
#include <string_view>
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

// a box of a JP2/JPX file: where it starts, the length of its header and its whole size
struct Jp2Box {
    std::size_t start;
    std::size_t header;
    std::size_t size;
    std::string_view type;
};

// the boxes from begin on, up to end or to the first bytes that aren't a box, whose position goes in stop
static std::vector<Jp2Box> read_boxes(const std::span<const uint8_t> data, std::size_t begin, const std::size_t end,
                                      std::size_t& stop) {
    std::vector<Jp2Box> boxes;
    while (begin < end) {
        if (end - begin < 8) break;
        uint64_t size = read_be32(data.data() + begin);
        std::size_t header = 8;
        if (size == 1) {
            if (end - begin < 16) break;
            size = read_be64(data.data() + begin + 8);
            header = 16;
        } else if (size == 0) {
            size = end - begin;
        }
        if (size < header || size > end - begin) break;
        boxes.push_back({begin, header, static_cast<std::size_t>(size),
                         std::string_view(reinterpret_cast<const char*>(data.data() + begin + 4), 4)});
        begin += static_cast<std::size_t>(size);
    }
    stop = begin;
    return boxes;
}

static void append_box(std::vector<uint8_t>& out, const std::string_view type, const std::span<const uint8_t> payload) {
    const auto put = [&out](const uint64_t value, const int bytes) {
        for (int i = bytes - 1; i >= 0; --i) out.push_back(static_cast<uint8_t>(value >> (8 * i)));
    };
    const uint64_t size = payload.size() + 8;
    if (size <= UINT32_MAX) {
        put(size, 4);
        out.insert(out.end(), type.begin(), type.end());
    } else {
        put(1, 4);
        out.insert(out.end(), type.begin(), type.end());
        put(size + 8, 8);
    }
    out.insert(out.end(), payload.begin(), payload.end());
}

// the marker segments of a codestream's main header, SOC excluded, or nothing if it doesn't parse
static std::optional<std::vector<std::span<const uint8_t>>> main_header(const std::span<const uint8_t> codestream) {
    if (codestream.size() < 2 || codestream[0] != 0xFF || codestream[1] != 0x4F) return std::nullopt;
    std::vector<std::span<const uint8_t>> segments;
    for (std::size_t p = 2;;) {
        if (codestream.size() - p < 4 || codestream[p] != 0xFF) return std::nullopt;
        // the first tile-part ends the main header
        if (codestream[p + 1] == 0x90) return segments;
        const std::size_t length = 2 + (static_cast<std::size_t>(codestream[p + 2]) << 8 | codestream[p + 3]);
        if (length < 4 || length > codestream.size() - p) return std::nullopt;
        segments.push_back(codestream.subspan(p, length));
        p += length;
    }
}

static bool is_comment(const std::span<const uint8_t> segment) {
    return segment[1] == 0x64;
}

// the codestream with the given comments in place of the ones openjpeg wrote
static std::optional<std::vector<uint8_t>> with_comments(const std::span<const uint8_t> codestream,
                                                         const std::vector<std::span<const uint8_t>>& comments) {
    const auto segments = main_header(codestream);
    if (!segments) return std::nullopt;
    std::vector<uint8_t> out(codestream.begin(), codestream.begin() + 2);
    std::size_t end = 2;
    for (const auto& segment : *segments) {
        end += segment.size();
        if (!is_comment(segment)) out.insert(out.end(), segment.begin(), segment.end());
    }
    for (const auto& comment : comments) out.insert(out.end(), comment.begin(), comment.end());
    out.insert(out.end(), codestream.begin() + static_cast<std::ptrdiff_t>(end), codestream.end());
    return out;
}

// XML, UUID (XMP, GeoJP2...), IPR and association (GMLJP2) boxes: what --no-meta drops
static bool is_metadata(const std::string_view type) {
    return type == "xml " || type == "uuid" || type == "uinf" || type == "jp2i" || type == "asoc" || type == "lbl ";
}

// the header box's content without its resolution box
static std::vector<uint8_t> header_without_resolution(const std::span<const uint8_t> data, const Jp2Box& jp2h) {
    std::size_t stop = 0;
    const auto children = read_boxes(data, jp2h.start + jp2h.header, jp2h.start + jp2h.size, stop);
    if (stop != jp2h.start + jp2h.size) {
        const auto payload = data.subspan(jp2h.start + jp2h.header, jp2h.size - jp2h.header);
        return {payload.begin(), payload.end()};
    }
    std::vector<uint8_t> out;
    for (const Jp2Box& child : children) {
        if (child.type == "res ") continue;
        const auto bytes = data.subspan(child.start, child.size);
        out.insert(out.end(), bytes.begin(), bytes.end());
    }
    return out;
}

void Jp2Processor::recompress(const std::filesystem::path& input_path,
                               const std::filesystem::path& output_path,
                               const ProcessingOptions& options) {
    Logger::log(LogLevel::Debug, "Entering recompress for " + input_path.string(), get_name());

    const OPJ_CODEC_FORMAT format = detect_codec_format(input_path);
    const std::vector<uint8_t> input = read_file(input_path);
    const std::span<const uint8_t> data(input);
    const auto leave_as_is = [&](const std::string& why) {
        Logger::log(LogLevel::Debug, why + ", left as is: " + input_path.string(), get_name());
        std::filesystem::copy_file(input_path, output_path, std::filesystem::copy_options::overwrite_existing);
    };

    // only the codestream changes: openjpeg understands a few of the boxes around it, so they're copied instead
    std::vector<Jp2Box> boxes;
    // bytes after the codestream that don't form boxes stay as they are
    std::size_t tail = data.size();
    std::span<const uint8_t> codestream = data;
    if (format == OPJ_CODEC_JP2) {
        boxes = read_boxes(data, 0, data.size(), tail);
        // fragment tables locate codestreams by offset, which would move
        if (std::ranges::any_of(boxes, [](const Jp2Box& b) { return b.type == "ftbl"; })) {
            return leave_as_is("Fragment table");
        }
        const auto jp2c = std::ranges::find_if(boxes, [](const Jp2Box& b) { return b.type == "jp2c"; });
        if (jp2c == boxes.end()) return leave_as_is("No codestream box");
        codestream = data.subspan(jp2c->start + jp2c->header, jp2c->size - jp2c->header);
    }
    const auto header = main_header(codestream);
    if (!header) return leave_as_is("Unreadable codestream header");

    // --- DECODER SETUP ---
    opj_dparameters_t dparam;
    opj_set_default_decoder_parameters(&dparam);
    // the components as stored: a palette or a channel definition stays in its box
    dparam.flags |= OPJ_DPARAMETERS_IGNORE_PCLR_CMAP_CDEF_FLAG;

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

    opj_codec_t* encoder = opj_create_compress(OPJ_CODEC_J2K);
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

    // openjpeg signs the codestream with a comment of its own: the input's comments go there instead
    std::vector<std::span<const uint8_t>> comments;
    if (options.preserve_metadata) {
        std::ranges::copy_if(*header, std::back_inserter(comments), is_comment);
    }
    const std::vector<uint8_t> encoded = read_file(output_path);
    auto recoded = with_comments(encoded, comments);
    if (!recoded) {
        throw std::runtime_error("Jp2Processor: unreadable codestream from openjpeg");
    }

    std::vector<uint8_t> out;
    if (format == OPJ_CODEC_J2K) {
        out = std::move(*recoded);
    } else {
        bool replaced = false;
        for (const Jp2Box& box : boxes) {
            if (box.type == "jp2c" && !replaced) {
                append_box(out, "jp2c", *recoded);
                replaced = true;
            } else if (!options.preserve_metadata && box.type == "jp2h") {
                append_box(out, "jp2h", header_without_resolution(data, box));
            } else if (options.preserve_metadata || !is_metadata(box.type)) {
                const auto bytes = data.subspan(box.start, box.size);
                out.insert(out.end(), bytes.begin(), bytes.end());
            }
        }
        out.insert(out.end(), input.begin() + static_cast<std::ptrdiff_t>(tail), input.end());
    }
    if (!write_file(output_path, out)) {
        throw std::runtime_error("Jp2Processor: can't write " + output_path.string());
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
