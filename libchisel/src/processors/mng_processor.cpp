//
// Created by Giuseppe Francione on 04/06/26.
//

#include "../../include/mng_processor.hpp"
#include "../../include/logger.hpp"
#include "../../include/file_utils.hpp"
#include "../../include/jpeg_transcode.hpp"
#include "../../include/zopfli_compressor.hpp"
#include <zlib.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace chisel {

namespace {

constexpr std::string_view kMngSignature("\x8AMNG\r\n\x1A\n", 8);
constexpr std::string_view kJngSignature("\x8BJNG\r\n\x1A\n", 8);
constexpr std::size_t kMaxChunkData = 0x7FFFFFFF;

/**
 * @brief One chunk, or a run of consecutive chunks of one stream type whose data forms a single stream.
 */
struct Segment {
    std::string type;
    std::vector<std::span<const uint8_t>> data; ///< data of each chunk of the run
    std::span<const uint8_t> stored;            ///< the chunks as stored, lengths and CRCs included
};

/**
 * @brief The chunks of an MNG or JNG file up to its closing chunk, and whatever follows it.
 */
struct Datastream {
    bool jng = false;
    std::vector<Segment> segments;
    std::span<const uint8_t> tail;
};

// zlib image data, and the JPEG image and alpha data of JNG images
bool is_stream(const std::string_view type) {
    return type == "IDAT" || type == "JDAT" || type == "JDAA";
}

/**
 * @brief Splits an MNG or JNG file into chunks, gathering each run of stream chunks.
 * @return std::nullopt if the signature, a chunk or the closing chunk is missing.
 */
std::optional<Datastream> parse(const std::span<const uint8_t> data) {
    if (data.size() < 8) return std::nullopt;
    Datastream ds;
    const std::string_view signature(reinterpret_cast<const char*>(data.data()), 8);
    if (signature == kJngSignature) {
        ds.jng = true;
    } else if (signature != kMngSignature) {
        return std::nullopt;
    }
    // an MNG ends with MEND: the IEND chunks inside it close its embedded images
    const std::string_view closing = ds.jng ? "IEND" : "MEND";
    std::size_t pos = 8;
    while (data.size() - pos >= 12) {
        const std::size_t length = read_be32(&data[pos]);
        if (length > data.size() - pos - 12) return std::nullopt;
        std::string type(reinterpret_cast<const char*>(&data[pos + 4]), 4);
        const auto chunk = data.subspan(pos, 12 + length);
        const auto payload = data.subspan(pos + 8, length);
        if (is_stream(type) && !ds.segments.empty() && ds.segments.back().type == type) {
            Segment& run = ds.segments.back();
            run.data.push_back(payload);
            run.stored = std::span(run.stored.data(), run.stored.size() + chunk.size());
        } else {
            ds.segments.push_back({.type = std::move(type), .data = {payload}, .stored = chunk});
        }
        pos += chunk.size();
        if (ds.segments.back().type == closing) {
            ds.tail = data.subspan(pos);
            return ds;
        }
    }
    return std::nullopt;
}

std::vector<uint8_t> joined(const Segment& run) {
    std::vector<uint8_t> out;
    for (const auto& d : run.data) out.insert(out.end(), d.begin(), d.end());
    return out;
}

/**
 * @brief Inflates a zlib stream that has to end exactly where @p in ends.
 */
std::optional<std::vector<uint8_t>> inflate_whole(const std::span<const uint8_t> in) {
    if (in.size() > UINT32_MAX) return std::nullopt;
    z_stream strm{};
    if (inflateInit(&strm) != Z_OK) return std::nullopt;
    strm.next_in = const_cast<Bytef*>(in.data());
    strm.avail_in = static_cast<uInt>(in.size());
    std::vector<uint8_t> out;
    std::array<uint8_t, 32768> buffer{};
    int ret = Z_OK;
    while (ret == Z_OK) {
        strm.next_out = buffer.data();
        strm.avail_out = static_cast<uInt>(buffer.size());
        ret = inflate(&strm, Z_NO_FLUSH);
        out.insert(out.end(), buffer.begin(), buffer.end() - strm.avail_out);
    }
    const bool whole = ret == Z_STREAM_END && strm.avail_in == 0;
    inflateEnd(&strm);
    if (!whole) return std::nullopt;
    return out;
}

/**
 * @brief SAVE can index SEEK chunks by their offset in the file, and re-encoding would move them.
 */
bool has_save_index(const Datastream& ds) {
    return std::ranges::any_of(ds.segments, [](const Segment& s) {
        return s.type == "SAVE" && !s.data.front().empty();
    });
}

/**
 * @brief Re-encodes the stream a run of chunks carries.
 * @return The smaller stream, or std::nullopt to keep the run as it is.
 */
std::optional<std::vector<uint8_t>> recompress_stream(const Segment& run, const ProcessingOptions& options) {
    const std::vector<uint8_t> stream = joined(run);
    std::vector<uint8_t> out;
    if (run.type == "IDAT") {
        const auto raw = inflate_whole(stream);
        if (!raw) return std::nullopt;
        out = ZopfliCompressor::compress(*raw, static_cast<unsigned>(options.iterations), ZopfliFormat::ZLIB);
    } else {
        // JHDR declares whether the JPEG data is sequential or progressive
        const jpeg::MarkerPolicy policy{.keep_all = options.preserve_metadata, .keep_links = false,
                                        .xmp = std::nullopt, .exif = std::nullopt};
        bool clean = false;
        if (!jpeg::transcode(stream, policy, jpeg::ScanMode::AsSource, out, clean) || !clean) {
            return std::nullopt;
        }
    }
    if (out.size() >= stream.size()) return std::nullopt;
    return out;
}

void append_chunks(std::vector<uint8_t>& out, const std::string_view type, const std::span<const uint8_t> data) {
    std::size_t done = 0;
    do {
        const std::size_t n = std::min(data.size() - done, kMaxChunkData);
        const std::size_t at = out.size();
        out.resize(at + 12 + n);
        write_be32(&out[at], static_cast<uint32_t>(n));
        std::memcpy(&out[at + 4], type.data(), 4);
        if (n > 0) std::memcpy(&out[at + 8], data.data() + done, n);
        const uLong crc = crc32(crc32(0L, Z_NULL, 0), &out[at + 4], static_cast<uInt>(4 + n));
        write_be32(&out[at + 8 + n], static_cast<uint32_t>(crc));
        done += n;
    } while (done < data.size());
}

} // namespace

void MngProcessor::recompress(const std::filesystem::path& input,
                              const std::filesystem::path& output,
                              const ProcessingOptions &options) {
    Logger::log(LogLevel::Debug, "Entering recompress for " + input.string(), get_name());

    std::vector<uint8_t> data;
    if (!read_file(input, data)) {
        throw std::runtime_error("MngProcessor: cannot open input");
    }

    std::vector<uint8_t> result;
    const auto ds = parse(data);
    if (!ds) {
        Logger::log(LogLevel::Warning, "Malformed MNG/JNG, leaving " + input.filename().string() + " unchanged",
                    get_name());
        result = data;
    } else if (has_save_index(*ds)) {
        Logger::log(LogLevel::Debug, "SAVE chunk with an index, leaving " + input.filename().string() + " unchanged",
                    get_name());
        result = data;
    } else {
        result.assign(data.begin(), data.begin() + 8);
        for (const Segment& segment : ds->segments) {
            const auto stream = is_stream(segment.type) ? recompress_stream(segment, options) : std::nullopt;
            if (stream) {
                append_chunks(result, segment.type, *stream);
            } else {
                result.insert(result.end(), segment.stored.begin(), segment.stored.end());
            }
        }
        result.insert(result.end(), ds->tail.begin(), ds->tail.end());
    }

    std::ofstream os(output, std::ios::binary | std::ios::trunc);
    if (!os) throw std::runtime_error("MngProcessor: can't write " + output.string());
    os.write(reinterpret_cast<const char*>(result.data()), static_cast<std::streamsize>(result.size()));
    os.close();
    if (os.fail()) throw std::runtime_error("MngProcessor: can't write " + output.string());

    Logger::log(LogLevel::Debug, "Exiting recompress for " + output.string(), get_name());
}

std::string MngProcessor::get_raw_checksum(const std::filesystem::path& /*file_path*/) const {
    return "";
}

bool MngProcessor::raw_equal(const std::filesystem::path& a, const std::filesystem::path& b) const {
    std::vector<uint8_t> da, db;
    if (!read_file(a, da) || !read_file(b, db)) return false;
    if (da == db) return true;
    const auto sa = parse(da);
    const auto sb = parse(db);
    if (!sa || !sb || sa->jng != sb->jng || sa->segments.size() != sb->segments.size() ||
        !std::ranges::equal(sa->tail, sb->tail)) {
        return false;
    }
    // other chunks are copied as they are; streams must carry the same image data
    for (std::size_t i = 0; i < sa->segments.size(); ++i) {
        const Segment& x = sa->segments[i];
        const Segment& y = sb->segments[i];
        if (x.type != y.type) return false;
        if (!is_stream(x.type)) {
            if (!std::ranges::equal(x.stored, y.stored)) return false;
            continue;
        }
        const std::vector<uint8_t> sx = joined(x);
        const std::vector<uint8_t> sy = joined(y);
        if (sx == sy) continue;
        if (x.type == "IDAT") {
            const auto ix = inflate_whole(sx);
            const auto iy = inflate_whole(sy);
            if (!ix || !iy || *ix != *iy) return false;
        } else {
            jpeg::Pixels px, py;
            if (!jpeg::decode_pixels(sx, px) || !jpeg::decode_pixels(sy, py) || px != py) return false;
        }
    }
    return true;
}

} // namespace chisel
