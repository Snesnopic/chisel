//
// Created by Giuseppe Francione on 23/10/26.
//

#include "../../include/tiff_rewriter.hpp"
#include "../../include/logger.hpp"
#include "../../include/options.hpp"
#include "../../include/thread_pool.hpp"
#include "../../include/zopfli_compressor.hpp"
#include <libdeflate.h>
#include <tiffio.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <vector>

namespace chisel {

namespace fs = std::filesystem;

// named, not anonymous: unity builds put this file next to others with constants of the same names
namespace tiff_internal {

constexpr uint16_t kCompression = 259;
constexpr uint16_t kStripOffsets = 273;
constexpr uint16_t kStripByteCounts = 279;
constexpr uint16_t kFreeOffsets = 288;
constexpr uint16_t kFreeByteCounts = 289;
constexpr uint16_t kPredictor = 317;
constexpr uint16_t kTileOffsets = 324;
constexpr uint16_t kTileByteCounts = 325;
constexpr uint16_t kSubIfds = 330;
constexpr uint16_t kGlobalParametersIfd = 400;
constexpr uint16_t kJpegInterchangeFormat = 513;
constexpr uint16_t kJpegInterchangeFormatLength = 514;
constexpr uint16_t kExifIfd = 34665;
constexpr uint16_t kGpsIfd = 34853;
constexpr uint16_t kMakerNote = 37500;
constexpr uint16_t kInteropIfd = 40965;
constexpr uint16_t kDngPrivateData = 50740;

constexpr uint16_t kTypeShort = 3;
constexpr uint16_t kTypeLong = 4;
constexpr uint16_t kTypeLong8 = 16;
constexpr uint16_t kDeflate = 8;
constexpr std::size_t kLargeBlock = 200000;  // same split between iterations and iterations_large as PDF streams

// --no-meta: camera, author and editing records, nothing that changes how the image looks or is read
constexpr std::array<uint16_t, 11> kMetadataTags = {271, 272, 305, 306, 315, 316, 700, 33432, 33723, kExifIfd, kGpsIfd};

// how image data is described: these entries may change, everything else stays byte for byte
bool describes_image_data(const uint16_t tag) {
    return tag == kCompression || tag == kPredictor || tag == kStripOffsets || tag == kStripByteCounts ||
           tag == kTileOffsets || tag == kTileByteCounts || tag == kJpegInterchangeFormat;
}

std::size_t type_size(const uint16_t type) {
    switch (type) {
        case 1: case 2: case 6: case 7: return 1;
        case 3: case 8: return 2;
        case 4: case 9: case 11: case 13: return 4;
        case 5: case 10: case 12: case 16: case 17: case 18: return 8;
        default: return 0;
    }
}

bool points_to_ifds(const uint16_t tag, const uint16_t type) {
    return tag == kSubIfds || tag == kGlobalParametersIfd || tag == kExifIfd || tag == kGpsIfd ||
           tag == kInteropIfd || type == 13 || type == 18;
}

struct Order {
    bool big = false;

    [[nodiscard]] uint64_t get(const uint8_t* p, const std::size_t n) const {
        uint64_t v = 0;
        for (std::size_t i = 0; i < n; ++i) v |= static_cast<uint64_t>(p[big ? n - 1 - i : i]) << (8 * i);
        return v;
    }
    void put(uint8_t* p, const std::size_t n, const uint64_t v) const {
        for (std::size_t i = 0; i < n; ++i) p[big ? n - 1 - i : i] = static_cast<uint8_t>(v >> (8 * i));
    }
};

class Input {
public:
    explicit Input(const fs::path& path) : in_(path, std::ios::binary) {
        std::error_code ec;
        size_ = fs::file_size(path, ec);
        if (ec) in_.setstate(std::ios::failbit);
    }
    [[nodiscard]] bool ok() const { return static_cast<bool>(in_); }
    [[nodiscard]] uint64_t size() const { return size_; }

    bool read(const uint64_t at, const uint64_t n, std::vector<uint8_t>& out) {
        if (at > size_ || n > size_ - at) return false;
        out.resize(static_cast<std::size_t>(n));
        in_.clear();
        in_.seekg(static_cast<std::streamoff>(at));
        in_.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(n));
        return static_cast<uint64_t>(in_.gcount()) == n;
    }

private:
    std::ifstream in_;
    uint64_t size_ = 0;
};

struct Ifd;

struct Entry {
    uint16_t tag = 0;
    uint16_t type = 0;
    uint64_t count = 0;
    std::vector<uint8_t> value;  ///< in the file's byte order
    std::vector<Ifd> subs;       ///< the IFDs it points to
};

struct Block {
    uint64_t offset = 0;
    uint64_t size = 0;
};

struct Ifd {
    std::vector<Entry> entries;
    std::vector<Block> data;     ///< strips or tiles
    uint16_t offsets_tag = 0;    ///< kStripOffsets or kTileOffsets, 0 without image data
    std::optional<Block> jpeg;   ///< JPEGInterchangeFormat thumbnail
};

struct Tiff {
    Order order;
    bool big = false;            ///< BigTIFF
    std::vector<Ifd> chain;
    uint64_t end = 0;            ///< end of the furthest byte the structure points at

    [[nodiscard]] std::size_t inline_size() const { return big ? 8 : 4; }
    [[nodiscard]] std::size_t count_size() const { return big ? 8 : 2; }
    [[nodiscard]] std::size_t entry_size() const { return big ? 20 : 12; }
};

uint64_t element(const Tiff& t, const Entry& e, const uint64_t k) {
    const std::size_t ts = type_size(e.type);
    return t.order.get(&e.value[static_cast<std::size_t>(k) * ts], ts);
}

class Parser {
public:
    Parser(Input& in, Tiff& t) : in_(in), t_(t) {}

    std::string parse() {
        std::vector<uint8_t> h;
        if (!in_.read(0, 8, h)) return "too short for a TIFF";
        if (h[0] == 'I' && h[1] == 'I') t_.order.big = false;
        else if (h[0] == 'M' && h[1] == 'M') t_.order.big = true;
        else return "not a TIFF";
        uint64_t first = 0;
        const uint64_t magic = t_.order.get(&h[2], 2);
        if (magic == 42) {
            first = t_.order.get(&h[4], 4);
            t_.end = 8;
        } else if (magic == 43) {
            if (!in_.read(0, 16, h) || t_.order.get(&h[4], 2) != 8 || t_.order.get(&h[6], 2) != 0) return "unsupported BigTIFF header";
            t_.big = true;
            first = t_.order.get(&h[8], 8);
            t_.end = 16;
        } else {
            return "not a TIFF";
        }
        std::set<uint64_t> seen;
        for (uint64_t at = first; at != 0;) {
            if (!seen.insert(at).second) return "IFD loop";
            Ifd ifd;
            uint64_t next = 0;
            if (auto err = parse_ifd(at, 0, ifd, next); !err.empty()) return err;
            t_.chain.push_back(std::move(ifd));
            at = next;
        }
        if (t_.chain.empty()) return "no IFD";
        return {};
    }

private:
    void mark(const uint64_t at, const uint64_t n) { t_.end = std::max(t_.end, at + n); }

    std::string parse_ifd(const uint64_t at, const int depth, Ifd& ifd, uint64_t& next) {
        if (depth > 8 || ++ifds_ > 100000) return "too many or too deeply nested IFDs";
        std::vector<uint8_t> buf;
        if (!in_.read(at, t_.count_size(), buf)) return "IFD past the end of the file";
        const uint64_t n = t_.order.get(buf.data(), t_.count_size());
        if (n == 0 || n > 65535) return "IFD with " + std::to_string(n) + " entries";
        const uint64_t table = n * t_.entry_size() + t_.inline_size();
        if (!in_.read(at + t_.count_size(), table, buf)) return "IFD past the end of the file";
        mark(at, t_.count_size() + table);
        next = t_.order.get(&buf[static_cast<std::size_t>(n * t_.entry_size())], t_.inline_size());

        for (uint64_t i = 0; i < n; ++i) {
            const uint8_t* e = &buf[static_cast<std::size_t>(i * t_.entry_size())];
            Entry entry;
            entry.tag = static_cast<uint16_t>(t_.order.get(e, 2));
            entry.type = static_cast<uint16_t>(t_.order.get(e + 2, 2));
            entry.count = t_.order.get(e + 4, t_.big ? 8 : 4);
            const std::string name = "entry " + std::to_string(entry.tag);
            const std::size_t ts = type_size(entry.type);
            if (ts == 0) return name + " has an unknown type";
            if (entry.count > (uint64_t{1} << 40) / ts) return name + " is too large";
            // their values hold offsets of their own, which moving them would leave pointing elsewhere
            if (entry.tag == kMakerNote || entry.tag == kDngPrivateData || entry.tag == kFreeOffsets ||
                entry.tag == kFreeByteCounts) {
                return name + " holds offsets of its own";
            }
            const uint64_t bytes = entry.count * ts;
            const uint8_t* field = e + (t_.big ? 12 : 8);
            if (bytes <= t_.inline_size()) {
                entry.value.assign(field, field + bytes);
            } else {
                const uint64_t off = t_.order.get(field, t_.inline_size());
                if (!in_.read(off, bytes, entry.value)) return name + " runs past the end of the file";
                mark(off, bytes);
            }
            if (points_to_ifds(entry.tag, entry.type)) {
                if (ts != 4 && ts != 8) return name + " points at IFDs with a type that can't";
                for (uint64_t k = 0; k < entry.count; ++k) {
                    Ifd sub;
                    uint64_t sub_next = 0;
                    if (auto err = parse_ifd(t_.order.get(&entry.value[static_cast<std::size_t>(k) * ts], ts), depth + 1, sub, sub_next);
                        !err.empty()) {
                        return err;
                    }
                    if (sub_next != 0) return name + " points at a chain of IFDs";
                    entry.subs.push_back(std::move(sub));
                }
            }
            ifd.entries.push_back(std::move(entry));
        }
        return collect_data(ifd);
    }

    std::string collect_data(Ifd& ifd) {
        const Entry* offsets = nullptr;
        const Entry* counts = nullptr;
        const Entry* jpeg = nullptr;
        const Entry* jpeg_length = nullptr;
        for (const auto& e : ifd.entries) {
            if (e.tag == kStripOffsets || e.tag == kTileOffsets) {
                if (offsets) return "IFD with both strips and tiles";
                offsets = &e;
            } else if (e.tag == kStripByteCounts || e.tag == kTileByteCounts) {
                if (counts) return "IFD with both strips and tiles";
                counts = &e;
            } else if (e.tag == kJpegInterchangeFormat) {
                jpeg = &e;
            } else if (e.tag == kJpegInterchangeFormatLength) {
                jpeg_length = &e;
            }
        }
        if (!offsets != !counts) return "image data without its sizes";
        if (offsets) {
            if ((offsets->tag == kStripOffsets) != (counts->tag == kStripByteCounts)) return "mixed strip and tile entries";
            if (offsets->count != counts->count) return "image data offsets and sizes don't match";
            const std::size_t os = type_size(offsets->type), cs = type_size(counts->type);
            if ((os != 2 && os != 4 && os != 8) || (cs != 2 && cs != 4 && cs != 8)) return "image data entries of an odd type";
            ifd.offsets_tag = offsets->tag;
            for (uint64_t k = 0; k < offsets->count; ++k) {
                const Block b{element(t_, *offsets, k), element(t_, *counts, k)};
                if (b.size != 0 && (b.offset > in_.size() || b.size > in_.size() - b.offset)) return "image data past the end of the file";
                mark(b.offset, b.size);
                ifd.data.push_back(b);
            }
        }
        if (!jpeg != !jpeg_length) return "thumbnail without its size";
        if (jpeg) {
            if (jpeg->count != 1 || jpeg_length->count != 1) return "odd thumbnail entries";
            const Block b{element(t_, *jpeg, 0), element(t_, *jpeg_length, 0)};
            if (b.offset > in_.size() || b.size > in_.size() - b.offset) return "thumbnail past the end of the file";
            mark(b.offset, b.size);
            ifd.jpeg = b;
        }
        return {};
    }

    Input& in_;
    Tiff& t_;
    std::size_t ifds_ = 0;
};

int quiet(TIFF*, void*, const char* module, const char* fmt, va_list ap) {
    std::array<char, 512> msg{};
    std::vsnprintf(msg.data(), msg.size(), fmt, ap);
    Logger::log(LogLevel::Debug, std::string(module ? module : "libtiff") + ": " + msg.data(), "TiffRewriter");
    return 1;
}

/**
 * @brief A libtiff handle that logs its warnings and errors instead of printing them.
 */
class TiffHandle {
public:
    explicit TiffHandle(const fs::path& path) {
        TIFFOpenOptions* opts = TIFFOpenOptionsAlloc();
        TIFFOpenOptionsSetErrorHandlerExtR(opts, quiet, nullptr);
        TIFFOpenOptionsSetWarningHandlerExtR(opts, quiet, nullptr);
        // "c": no strip chopping, which would split a large uncompressed strip into ones the file doesn't have
        tif_ = TIFFOpenExt(path.string().c_str(), "rc", opts);
        TIFFOpenOptionsFree(opts);
    }
    ~TiffHandle() { if (tif_) TIFFClose(tif_); }
    TiffHandle(const TiffHandle&) = delete;
    TiffHandle& operator=(const TiffHandle&) = delete;
    [[nodiscard]] TIFF* get() const { return tif_; }

private:
    TIFF* tif_ = nullptr;
};

/**
 * @brief Decodes every strip or tile of the current directory, samples in host byte order.
 */
std::optional<std::vector<std::vector<uint8_t>>> decode_blocks(TIFF* tif, const std::size_t expected) {
    const bool tiled = TIFFIsTiled(tif);
    const uint32_t count = tiled ? TIFFNumberOfTiles(tif) : TIFFNumberOfStrips(tif);
    if (count != expected || count == 0) return std::nullopt;
    const tmsize_t max_size = tiled ? TIFFTileSize(tif) : TIFFStripSize(tif);
    if (max_size <= 0) return std::nullopt;
    std::vector<std::vector<uint8_t>> blocks(count);
    for (uint32_t b = 0; b < count; ++b) {
        blocks[b].resize(static_cast<std::size_t>(max_size));
        const tmsize_t got = tiled ? TIFFReadEncodedTile(tif, b, blocks[b].data(), max_size)
                                   : TIFFReadEncodedStrip(tif, b, blocks[b].data(), max_size);
        if (got < 0) return std::nullopt;
        blocks[b].resize(static_cast<std::size_t>(got));
    }
    return blocks;
}

template <typename T>
void differentiate(uint8_t* row, const std::size_t samples, const std::size_t stride) {
    for (std::size_t i = samples; i-- > stride;) {
        T v, prev;
        std::memcpy(&v, row + i * sizeof(T), sizeof(T));
        std::memcpy(&prev, row + (i - stride) * sizeof(T), sizeof(T));
        v = static_cast<T>(v - prev);
        std::memcpy(row + i * sizeof(T), &v, sizeof(T));
    }
}

std::vector<uint8_t> zlib(const std::span<const uint8_t> data) {
    libdeflate_compressor* c = libdeflate_alloc_compressor(12);
    if (!c) return {};
    std::vector<uint8_t> out(libdeflate_zlib_compress_bound(c, data.size()));
    const std::size_t n = libdeflate_zlib_compress(c, data.data(), data.size(), out.data(), out.size());
    libdeflate_free_compressor(c);
    out.resize(n);
    return out;
}

void for_each_block(const std::size_t n, const std::function<void(std::size_t)>& body) {
    if (ThreadPool* pool = ThreadPool::current()) {
        pool->parallel_for(n, body);
    } else {
        for (std::size_t i = 0; i < n; ++i) body(i);
    }
}

/**
 * @brief The image data of one IFD, recompressed.
 */
struct Encoded {
    std::vector<std::vector<uint8_t>> blocks;
    uint16_t predictor = 1;
};

/**
 * @brief Recompresses the image data of libtiff's current directory, if that can be done exactly and gets smaller.
 */
std::optional<Encoded> encode(TIFF* tif, const Ifd& ifd, const Order& order, const ProcessingOptions& options) {
    if (ifd.data.empty()) return std::nullopt;
    uint64_t original = 0;
    for (const auto& b : ifd.data) {
        if (b.size == 0) return std::nullopt;  // sparse image data
        original += b.size;
    }

    uint16_t compression = COMPRESSION_NONE, fill_order = FILLORDER_MSB2LSB, bits = 1, spp = 1;
    uint16_t format = SAMPLEFORMAT_UINT, planar = PLANARCONFIG_CONTIG, photometric = 0;
    TIFFGetFieldDefaulted(tif, TIFFTAG_COMPRESSION, &compression);
    TIFFGetFieldDefaulted(tif, TIFFTAG_FILLORDER, &fill_order);
    TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &bits);
    TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &spp);
    TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLEFORMAT, &format);
    TIFFGetFieldDefaulted(tif, TIFFTAG_PLANARCONFIG, &planar);
    TIFFGetField(tif, TIFFTAG_PHOTOMETRIC, &photometric);

    // only lossless codecs whose decoded bytes are the samples themselves
    switch (compression) {
        case COMPRESSION_NONE: case COMPRESSION_LZW: case COMPRESSION_ADOBE_DEFLATE:
        case COMPRESSION_DEFLATE: case COMPRESSION_PACKBITS:
            break;
        default:
            return std::nullopt;
    }
    if (fill_order != FILLORDER_MSB2LSB) return std::nullopt;
    if (bits != 1 && bits != 2 && bits != 4 && bits != 8 && bits != 16 && bits != 32 && bits != 64) return std::nullopt;
    if (photometric == PHOTOMETRIC_YCBCR) {
        uint16_t h = 1, v = 1;
        TIFFGetFieldDefaulted(tif, TIFFTAG_YCBCRSUBSAMPLING, &h, &v);
        if (h != 1 || v != 1) return std::nullopt;
    }
    if ((TIFFIsTiled(tif) != 0) != (ifd.offsets_tag == kTileOffsets)) return std::nullopt;
    // Apple's ImageIO reads Deflate strips but not Deflate tiles: tiles only stay Deflate
    if (TIFFIsTiled(tif) && compression != COMPRESSION_ADOBE_DEFLATE && compression != COMPRESSION_DEFLATE) return std::nullopt;

    auto decoded = decode_blocks(tif, ifd.data.size());
    if (!decoded) return std::nullopt;

    uint32_t width = 0, tile_width = 0;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &width);
    if (TIFFIsTiled(tif)) TIFFGetField(tif, TIFFTAG_TILEWIDTH, &tile_width);
    const std::size_t sample_bytes = bits >= 8 ? bits / 8 : 0;
    const std::size_t stride = planar == PLANARCONFIG_CONTIG ? spp : 1;
    const std::size_t row_samples = static_cast<std::size_t>(TIFFIsTiled(tif) ? tile_width : width) * stride;
    const std::size_t row_bytes = row_samples * sample_bytes;
    bool can_predict = (bits == 8 || bits == 16 || bits == 32) && row_bytes != 0 &&
                       (format == SAMPLEFORMAT_UINT || format == SAMPLEFORMAT_INT) && photometric != PHOTOMETRIC_YCBCR;
    for (const auto& b : *decoded) can_predict = can_predict && b.size() % row_bytes == 0;
    // libtiff hands over samples in host order; the file wants its own
    const bool swap = sample_bytes > 1 && order.big != (std::endian::native == std::endian::big);

    const auto prepare = [&](const std::vector<uint8_t>& block, const bool predict) {
        std::vector<uint8_t> out = block;
        if (predict) {
            for (std::size_t row = 0; row < out.size(); row += row_bytes) {
                if (bits == 8) differentiate<uint8_t>(&out[row], row_samples, stride);
                else if (bits == 16) differentiate<uint16_t>(&out[row], row_samples, stride);
                else differentiate<uint32_t>(&out[row], row_samples, stride);
            }
        }
        if (swap) {
            for (std::size_t i = 0; i + sample_bytes <= out.size(); i += sample_bytes) {
                std::reverse(out.begin() + static_cast<std::ptrdiff_t>(i), out.begin() + static_cast<std::ptrdiff_t>(i + sample_bytes));
            }
        }
        return out;
    };

    // pick the predictor with a fast pass, then spend zopfli's time on the winner
    const std::size_t n = decoded->size();
    std::vector<std::size_t> plain_size(n), predicted_size(n);
    for_each_block(n, [&](const std::size_t b) {
        plain_size[b] = zlib(prepare((*decoded)[b], false)).size();
        predicted_size[b] = can_predict ? zlib(prepare((*decoded)[b], true)).size() : SIZE_MAX;
    });
    uint64_t plain_total = 0, predicted_total = 0;
    for (std::size_t b = 0; b < n; ++b) {
        if (plain_size[b] == 0 || predicted_size[b] == 0) return std::nullopt;
        plain_total += plain_size[b];
        if (can_predict) predicted_total += predicted_size[b];
    }
    Encoded encoded;
    encoded.predictor = can_predict && predicted_total < plain_total ? 2 : 1;
    encoded.blocks.resize(n);
    for_each_block(n, [&](const std::size_t b) {
        const auto data = prepare((*decoded)[b], encoded.predictor == 2);
        auto best = zlib(data);
        const unsigned iterations = static_cast<unsigned>(data.size() < kLargeBlock ? options.iterations : options.iterations_large);
        if (auto zopfli = ZopfliCompressor::compress(data, iterations, ZopfliFormat::ZLIB);
            !zopfli.empty() && zopfli.size() < best.size()) {
            best = std::move(zopfli);
        }
        encoded.blocks[b] = std::move(best);
    });
    uint64_t total = 0;
    for (const auto& b : encoded.blocks) {
        if (b.empty()) return std::nullopt;
        total += b.size();
    }
    if (total >= original) return std::nullopt;
    return encoded;
}

class Output {
public:
    explicit Output(const fs::path& path) : out_(path, std::ios::binary | std::ios::trunc) {}
    [[nodiscard]] bool ok() const { return static_cast<bool>(out_); }
    [[nodiscard]] uint64_t pos() const { return pos_; }

    void write(const uint8_t* p, const std::size_t n) {
        out_.write(reinterpret_cast<const char*>(p), static_cast<std::streamsize>(n));
        pos_ += n;
    }
    void align() {
        if (pos_ % 2 != 0) {
            constexpr uint8_t zero = 0;
            write(&zero, 1);
        }
    }
    void patch(const uint64_t at, const uint8_t* p, const std::size_t n) {
        out_.seekp(static_cast<std::streamoff>(at));
        out_.write(reinterpret_cast<const char*>(p), static_cast<std::streamsize>(n));
        out_.seekp(static_cast<std::streamoff>(pos_));
    }
    bool copy(Input& in, uint64_t at, uint64_t n) {
        std::vector<uint8_t> chunk;
        while (n > 0) {
            const uint64_t take = std::min<uint64_t>(n, 1 << 22);
            if (!in.read(at, take, chunk)) return false;
            write(chunk.data(), chunk.size());
            at += take;
            n -= take;
        }
        return true;
    }

private:
    std::ofstream out_;
    uint64_t pos_ = 0;
};

class Emitter {
public:
    Emitter(Input& in, Output& out, const Tiff& t, const std::set<uint16_t>& drop) : in_(in), out_(out), t_(t), drop_(drop) {}

    /**
     * @brief Writes an IFD with its data, values and sub-IFDs; @p table_at and @p next_at say where its table
     * and next IFD pointer went.
     */
    bool emit(const Ifd& ifd, const Encoded* encoded, uint64_t& table_at, uint64_t& next_at) {
        // image data first, so that the entries can point at it
        std::vector<uint64_t> offsets, sizes;
        for (std::size_t b = 0; b < ifd.data.size(); ++b) {
            if (encoded) {
                out_.align();
                offsets.push_back(out_.pos());
                sizes.push_back(encoded->blocks[b].size());
                out_.write(encoded->blocks[b].data(), encoded->blocks[b].size());
            } else if (ifd.data[b].size == 0) {
                offsets.push_back(0);
                sizes.push_back(0);
            } else {
                out_.align();
                offsets.push_back(out_.pos());
                sizes.push_back(ifd.data[b].size);
                if (!out_.copy(in_, ifd.data[b].offset, ifd.data[b].size)) return false;
            }
        }
        uint64_t jpeg_at = 0;
        if (ifd.jpeg) {
            out_.align();
            jpeg_at = out_.pos();
            if (!out_.copy(in_, ifd.jpeg->offset, ifd.jpeg->size)) return false;
        }

        std::vector<Entry> entries;
        bool has_compression = false, has_predictor = false;
        for (const auto& e : ifd.entries) {
            if (drop_.contains(e.tag)) continue;
            Entry n{e.tag, e.type, e.count, e.value, {}};
            if (!e.subs.empty()) {
                const std::size_t ts = type_size(e.type);
                for (std::size_t k = 0; k < e.subs.size(); ++k) {
                    uint64_t sub_table = 0, sub_next = 0;
                    if (!emit(e.subs[k], nullptr, sub_table, sub_next)) return false;
                    if (!fits(sub_table, ts)) return false;
                    t_.order.put(&n.value[k * ts], ts, sub_table);
                }
            } else if (e.tag == kStripOffsets || e.tag == kTileOffsets) {
                if (!set_array(n, offsets)) return false;
            } else if (e.tag == kStripByteCounts || e.tag == kTileByteCounts) {
                if (!set_array(n, sizes)) return false;
            } else if (e.tag == kJpegInterchangeFormat) {
                if (!fits(jpeg_at, type_size(n.type))) return false;
                t_.order.put(n.value.data(), type_size(n.type), jpeg_at);
            } else if (encoded && e.tag == kCompression && e.count == 1) {
                t_.order.put(n.value.data(), type_size(n.type), kDeflate);
                has_compression = true;
            } else if (encoded && e.tag == kPredictor && e.count == 1) {
                t_.order.put(n.value.data(), type_size(n.type), encoded->predictor);
                has_predictor = true;
            }
            entries.push_back(std::move(n));
        }
        // a missing Compression means none, a missing Predictor means none
        if (encoded && !has_compression) insert_short(entries, kCompression, kDeflate);
        if (encoded && !has_predictor && encoded->predictor != 1) insert_short(entries, kPredictor, encoded->predictor);

        // values too long for their entry, then the table
        std::vector<uint64_t> value_at(entries.size(), 0);
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].value.size() <= t_.inline_size()) continue;
            out_.align();
            value_at[i] = out_.pos();
            out_.write(entries[i].value.data(), entries[i].value.size());
        }
        out_.align();
        table_at = out_.pos();
        const std::size_t cs = t_.count_size(), es = t_.entry_size(), is = t_.inline_size();
        std::vector<uint8_t> table(cs + entries.size() * es + is, 0);
        t_.order.put(table.data(), cs, entries.size());
        for (std::size_t i = 0; i < entries.size(); ++i) {
            uint8_t* e = &table[cs + i * es];
            t_.order.put(e, 2, entries[i].tag);
            t_.order.put(e + 2, 2, entries[i].type);
            t_.order.put(e + 4, t_.big ? 8 : 4, entries[i].count);
            uint8_t* field = e + (t_.big ? 12 : 8);
            if (entries[i].value.size() <= is) {
                std::ranges::copy(entries[i].value, field);
            } else {
                if (!fits(value_at[i], is)) return false;
                t_.order.put(field, is, value_at[i]);
            }
        }
        if (!fits(table_at, is)) return false;
        next_at = table_at + cs + entries.size() * es;
        out_.write(table.data(), table.size());
        return out_.ok();
    }

private:
    void insert_short(std::vector<Entry>& entries, const uint16_t tag, const uint16_t value) const {
        Entry e{tag, kTypeShort, 1, std::vector<uint8_t>(2), {}};
        t_.order.put(e.value.data(), 2, value);
        const auto at = std::ranges::find_if(entries, [tag](const Entry& x) { return x.tag > tag; });
        entries.insert(at, std::move(e));
    }

    static bool fits(const uint64_t v, const std::size_t bytes) {
        return bytes >= 8 || v < (uint64_t{1} << (8 * bytes));
    }

    // strip or tile offsets and sizes, widening SHORT entries when the new values need it
    bool set_array(Entry& e, const std::vector<uint64_t>& values) const {
        std::size_t ts = type_size(e.type);
        const uint64_t largest = values.empty() ? 0 : *std::ranges::max_element(values);
        if (!fits(largest, ts)) {
            e.type = t_.big ? kTypeLong8 : kTypeLong;
            ts = type_size(e.type);
            if (!fits(largest, ts)) return false;
        }
        e.value.assign(values.size() * ts, 0);
        for (std::size_t k = 0; k < values.size(); ++k) t_.order.put(&e.value[k * ts], ts, values[k]);
        return true;
    }

    Input& in_;
    Output& out_;
    const Tiff& t_;
    const std::set<uint16_t>& drop_;
};

std::string parse_file(const fs::path& path, Input& in, Tiff& t) {
    if (!in.ok()) return "can't be read";
    return Parser(in, t).parse();
}

bool same_bytes(Input& a, const Block& x, Input& b, const Block& y) {
    if (x.size != y.size) return false;
    std::vector<uint8_t> ca, cb;
    for (uint64_t done = 0; done < x.size;) {
        const uint64_t take = std::min<uint64_t>(x.size - done, 1 << 22);
        if (!a.read(x.offset + done, take, ca) || !b.read(y.offset + done, take, cb) || ca != cb) return false;
        done += take;
    }
    return true;
}

std::string compare_entries(const Ifd& a, const Ifd& b, const std::set<uint16_t>& ignore, const std::string& where) {
    const auto relevant = [&](const Ifd& ifd) {
        std::vector<const Entry*> out;
        for (const auto& e : ifd.entries) {
            if (!describes_image_data(e.tag) && !ignore.contains(e.tag)) out.push_back(&e);
        }
        return out;
    };
    const auto ea = relevant(a), eb = relevant(b);
    if (ea.size() != eb.size()) return where + ": " + std::to_string(ea.size()) + " entries became " + std::to_string(eb.size());
    for (std::size_t i = 0; i < ea.size(); ++i) {
        const Entry& x = *ea[i];
        const Entry& y = *eb[i];
        const std::string name = where + " entry " + std::to_string(x.tag);
        if (x.tag != y.tag || x.type != y.type || x.count != y.count) return name + " changed tag, type or count";
        if (x.subs.size() != y.subs.size()) return name + " points at a different number of IFDs";
        if (x.subs.empty()) {
            if (x.value != y.value) return name + " changed value";
            continue;
        }
        for (std::size_t k = 0; k < x.subs.size(); ++k) {
            if (auto diff = compare_entries(x.subs[k], y.subs[k], {}, name); !diff.empty()) return diff;
        }
    }
    return {};
}

// data of IFDs libtiff doesn't decode, sub-IFD images and thumbnails: compared as they're stored
std::string compare_stored(Input& ia, const Ifd& a, Input& ib, const Ifd& b, const std::string& where) {
    if (a.data.size() != b.data.size()) return where + ": different number of strips or tiles";
    for (std::size_t k = 0; k < a.data.size(); ++k) {
        if (!same_bytes(ia, a.data[k], ib, b.data[k])) return where + ": different image data";
    }
    if (a.jpeg.has_value() != b.jpeg.has_value() || (a.jpeg && !same_bytes(ia, *a.jpeg, ib, *b.jpeg))) return where + ": different thumbnail";
    for (const auto& x : a.entries) {
        if (x.subs.empty()) continue;
        const auto y = std::ranges::find_if(b.entries, [&](const Entry& e) { return e.tag == x.tag; });
        if (y == b.entries.end() || y->subs.size() != x.subs.size()) continue;  // compare_entries reports it
        for (std::size_t k = 0; k < x.subs.size(); ++k) {
            if (auto diff = compare_stored(ia, x.subs[k], ib, y->subs[k], where + " entry " + std::to_string(x.tag)); !diff.empty()) return diff;
        }
    }
    return {};
}

std::string compare(const fs::path& pa, const fs::path& pb, const bool with_tags, const std::set<uint16_t>& ignore) {
    Input ia(pa), ib(pb);
    Tiff ta, tb;
    if (auto err = parse_file(pa, ia, ta); !err.empty()) return "the original " + err;
    if (auto err = parse_file(pb, ib, tb); !err.empty()) return "the rewritten file " + err;
    if (ta.order.big != tb.order.big || ta.big != tb.big) return "different byte order or layout";
    if (ta.chain.size() != tb.chain.size()) return std::to_string(ta.chain.size()) + " IFDs became " + std::to_string(tb.chain.size());

    TiffHandle ha(pa), hb(pb);
    for (std::size_t i = 0; i < ta.chain.size(); ++i) {
        const Ifd& a = ta.chain[i];
        const Ifd& b = tb.chain[i];
        const std::string where = "IFD " + std::to_string(i);
        if (with_tags) {
            if (auto diff = compare_entries(a, b, ignore, where); !diff.empty()) return diff;
        }
        if (a.data.size() != b.data.size()) return where + ": different number of strips or tiles";
        bool decoded = false;
        if (!a.data.empty() && ha.get() && hb.get() && TIFFSetDirectory(ha.get(), static_cast<tdir_t>(i)) &&
            TIFFSetDirectory(hb.get(), static_cast<tdir_t>(i))) {
            const auto da = decode_blocks(ha.get(), a.data.size());
            if (da) {
                const auto db = decode_blocks(hb.get(), b.data.size());
                if (!db || *da != *db) return where + ": different decoded image data";
                decoded = true;
            }
        }
        Ifd a_rest = a, b_rest = b;
        if (decoded) {
            a_rest.data.clear();
            b_rest.data.clear();
        }
        if (auto diff = compare_stored(ia, a_rest, ib, b_rest, where); !diff.empty()) return diff;
    }
    return {};
}

} // namespace tiff_internal

TiffRewriteResult rewrite_tiff(const fs::path& input, const fs::path& output, const ProcessingOptions& options) {
    using namespace tiff_internal;
    TiffRewriteResult result;
    Input in(input);
    Tiff t;
    if (auto err = parse_file(input, in, t); !err.empty()) {
        result.reason = err;
        return result;
    }

    std::set<uint16_t> drop;
    if (!options.preserve_metadata) {
        for (const auto& ifd : t.chain) {
            for (const auto& e : ifd.entries) {
                if (std::ranges::find(kMetadataTags, e.tag) != kMetadataTags.end()) drop.insert(e.tag);
            }
        }
    }

    std::vector<std::optional<Encoded>> encoded(t.chain.size());
    {
        TiffHandle h(input);
        if (h.get()) {
            for (std::size_t i = 0; i < t.chain.size(); ++i) {
                if (!TIFFSetDirectory(h.get(), static_cast<tdir_t>(i))) break;
                encoded[i] = encode(h.get(), t.chain[i], t.order, options);
            }
        }
    }
    if (drop.empty() && std::ranges::none_of(encoded, [](const auto& e) { return e.has_value(); })) {
        result.reason = "no image data that gets smaller";
        return result;
    }

    {
        Output out(output);
        if (!out.ok()) {
            result.reason = "the output can't be written";
            return result;
        }
        std::vector<uint8_t> header(t.big ? 16 : 8, 0);
        header[0] = header[1] = t.order.big ? 'M' : 'I';
        t.order.put(&header[2], 2, t.big ? 43 : 42);
        if (t.big) t.order.put(&header[4], 2, 8);
        out.write(header.data(), header.size());

        Emitter emitter(in, out, t, drop);
        uint64_t pointer_at = t.big ? 8 : 4;
        for (std::size_t i = 0; i < t.chain.size(); ++i) {
            uint64_t table_at = 0, next_at = 0;
            if (!emitter.emit(t.chain[i], encoded[i] ? &*encoded[i] : nullptr, table_at, next_at)) {
                result.reason = "the rewritten layout doesn't fit";
                break;
            }
            std::array<uint8_t, 8> p{};
            t.order.put(p.data(), t.inline_size(), table_at);
            out.patch(pointer_at, p.data(), t.inline_size());
            pointer_at = next_at;
        }
        // whatever follows the structure (padding, appended data) stays as it was
        if (result.reason.empty() && in.size() > t.end && !out.copy(in, t.end, in.size() - t.end)) {
            result.reason = "the original can't be read";
        }
        if (result.reason.empty() && !out.ok()) result.reason = "the output can't be written";
    }
    if (!result.reason.empty()) {
        std::error_code ec;
        fs::remove(output, ec);
        return result;
    }

    // only a file that reads back as the original may replace it
    if (auto diff = compare(input, output, true, drop); !diff.empty()) {
        std::error_code ec;
        fs::remove(output, ec);
        result.mismatch = true;
        result.reason = diff;
        return result;
    }
    result.written = true;
    return result;
}

std::string compare_tiff(const fs::path& a, const fs::path& b, const bool with_tags) {
    return tiff_internal::compare(a, b, with_tags, {});
}

} // namespace chisel
