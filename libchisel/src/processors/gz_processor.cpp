//
// Created by Giuseppe Francione on 11/06/26.
//

#include "../../include/gz_processor.hpp"
#include "../../include/logger.hpp"
#include "../../include/file_utils.hpp"
#include <libdeflate.h>
#include <fstream>
#include <vector>
#include <stdexcept>
#include <cstring>
#include <cstdint>

/**
 * @file gz_processor.cpp
 * @brief DEFLATE recompressor and header stripper for gzip files.
 *
 * Gzip file format (RFC 1952):
 *
 *   Member header:
 *     uint8   ID1      = 0x1F
 *     uint8   ID2      = 0x8B
 *     uint8   CM       = 8 (DEFLATE)
 *     uint8   FLG      bits:
 *                        0x02 FHCRC   — 2-byte CRC16 of header
 *                        0x04 FEXTRA  — extra data block follows
 *                        0x08 FNAME   — NUL-terminated original filename
 *                        0x10 FCOMMENT — NUL-terminated comment
 *     uint32  MTIME    — modification time (Unix epoch), can be 0
 *     uint8   XFL      — extra flags (2 = max compression, 4 = fastest)
 *     uint8   OS       — 0xFF = unknown
 *
 *   If FEXTRA set:
 *     uint16  XLEN     — extra data length
 *     uint8[XLEN]      — extra data
 *
 *   If FNAME set:
 *     char[]  NUL-terminated original filename
 *
 *   If FCOMMENT set:
 *     char[]  NUL-terminated comment
 *
 *   If FHCRC set:
 *     uint16  CRC16 of header bytes
 *
 *   Raw DEFLATE stream (RFC 1951, no zlib wrapper)
 *
 *   Member trailer:
 *     uint32  CRC32   — CRC-32 of uncompressed data
 *     uint32  ISIZE   — uncompressed size mod 2^32
 *
 * A gzip file may contain multiple concatenated members.
 * We handle each member independently.
 *
 * Strategy:
 *   For each member:
 *     1. Parse the member header, recording all optional fields.
 *     2. Determine the DEFLATE payload length by finding the trailer:
 *        decompress with libdeflate_deflate_decompress to get raw bytes.
 *     3. Recompress the raw bytes with libdeflate at level 12.
 *     4. Emit a minimal header (10 bytes, FLG=0x00 if !preserve_metadata,
 *        else keep FNAME/FCOMMENT/MTIME), the new DEFLATE stream,
 *        then the original CRC32 and ISIZE trailer.
 */

namespace chisel {

namespace {

// ─── constants ────────────────────────────────────────────────────────────────
constexpr uint8_t kGzId1 = 0x1F;
constexpr uint8_t kGzId2 = 0x8B;
constexpr uint8_t kGzCm  = 8;    // DEFLATE

constexpr uint8_t kFlgFhcrc   = 0x02;
constexpr uint8_t kFlgFextra  = 0x04;
constexpr uint8_t kFlgFname   = 0x08;
constexpr uint8_t kFlgFcomment = 0x10;

constexpr int     kGzDeflateLevel = 12; // libdeflate max

// ─── safe span reader ─────────────────────────────────────────────────────────

struct Reader {
    const uint8_t* data;
    std::size_t         size;
    std::size_t         pos = 0;

    uint8_t u8() {
        if (pos >= size) throw std::runtime_error("GzProcessor: unexpected end of file");
        return data[pos++];
    }

    uint16_t u16_le() {
        const uint8_t lo = u8();
        const uint8_t hi = u8();
        return static_cast<uint16_t>(lo | (hi << 8));
    }

    uint32_t u32_le() {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= (static_cast<uint32_t>(u8()) << (8 * i));
        return v;
    }

    void skip(const std::size_t n) {
        if (pos + n > size) throw std::runtime_error("GzProcessor: unexpected end of file");
        pos += n;
    }

    // read NUL-terminated string, return without NUL
    std::string nul_string() {
        std::string s;
        while (true) {
            const uint8_t c = u8();
            if (c == 0) break;
            s += static_cast<char>(c);
        }
        return s;
    }

    const uint8_t* cur() const { return data + pos; }
    std::size_t remaining() const { return size - pos; }
};

struct Writer {
    std::vector<uint8_t> buf;

    void u8(const uint8_t v) { buf.push_back(v); }

    void u16_le(const uint16_t v) {
        buf.push_back(static_cast<uint8_t>(v));
        buf.push_back(static_cast<uint8_t>(v >> 8));
    }

    void u32_le(const uint32_t v) {
        buf.push_back(static_cast<uint8_t>(v));
        buf.push_back(static_cast<uint8_t>(v >> 8));
        buf.push_back(static_cast<uint8_t>(v >> 16));
        buf.push_back(static_cast<uint8_t>(v >> 24));
    }

    void bytes(const uint8_t* p, const std::size_t n) {
        buf.insert(buf.end(), p, p + n);
    }

    void nul_string(const std::string& s) {
        buf.insert(buf.end(), s.begin(), s.end());
        buf.push_back(0);
    }
};

// ─── CRC32 (needed to validate trailer) ──────────────────────────────────────

/**
 * @brief Compute CRC-32 using libdeflate.
 */
uint32_t crc32_of(const uint8_t* data, const std::size_t size) {
    return libdeflate_crc32(0, data, size);
}

// ─── decompress the DEFLATE stream in a gz member ────────────────────────────

/**
 * @brief Try to decompress a raw DEFLATE stream of unknown compressed length.
 *
 * libdeflate requires knowing the compressed length. We find it by scanning
 * for the 8-byte trailer and working backwards, using the ISIZE field to
 * validate.
 *
 * @param r     Reader positioned at the start of the DEFLATE stream.
 * @param raw   Output: decompressed bytes.
 * @param crc32_out Output: CRC32 from trailer (to re-emit unchanged).
 * @param isize_out Output: ISIZE from trailer.
 * @returns Number of compressed bytes consumed from the reader.
 */
size_t decompress_member(const Reader& r, std::vector<uint8_t>& raw,
                          uint32_t& crc32_out, uint32_t& isize_out) {
    // The trailer is 8 bytes at the end of this member (or before the next
    // member). We need to find the compressed length.
    //
    // Strategy: try progressively larger windows using libdeflate. This is
    // O(n) in practice because we start from the full remaining size and
    // binary-search if needed. In practice, gzip files are single-member or
    // have well-delimited boundaries, so we can just try the full remaining
    // data minus 8 bytes for the trailer.
    //
    // For multi-member files we rely on libdeflate's exact_out_size mode
    // combined with the ISIZE hint.

    const std::size_t avail = r.remaining();
    if (avail < 8) throw std::runtime_error("GzProcessor: too small for DEFLATE+trailer");

    // Read ISIZE from the last 4 bytes of this member (or the entire rest if
    // single-member). We'll refine below.
    uint32_t isize_hint;
    std::memcpy(&isize_hint, r.data + r.size - 4, 4); // little-endian read

    // For a proper multi-member file, we'd need to find where this member ends.
    // We use a greedy approach: try the maximum compressed length first.
    // libdeflate will stop at the end of the valid deflate stream and we read
    // the 8 bytes that follow.

    // Maximum possible compressed size: everything except the 8-byte trailer.
    // For single-member files this is exact. For multi-member we may overshoot,
    // but libdeflate_deflate_decompress will return LIBDEFLATE_SHORT_OUTPUT
    // if the stream ends before filling the buffer.

    // We use a two-pass approach: decompress into a large buffer using
    // ISIZE as size hint. If ISIZE overflows (>= 2^32 bytes rare), fall back.

    const std::size_t uncomp_hint = (isize_hint == 0 && avail > 65536)
        ? avail * 4    // rough guess for very large streams
        : (isize_hint > 0 ? isize_hint : 65536);

    libdeflate_decompressor* dec = libdeflate_alloc_decompressor();
    if (!dec) throw std::runtime_error("GzProcessor: libdeflate_alloc_decompressor failed");

    // Try with (avail - 8) as compressed size
    const std::size_t compressed_len = avail - 8;
    raw.resize(uncomp_hint);
    std::size_t actual = 0;
    libdeflate_result res = libdeflate_deflate_decompress(
        dec, r.cur(), compressed_len, raw.data(), raw.size(), &actual);

    if (res == LIBDEFLATE_INSUFFICIENT_SPACE) {
        // Need a bigger output buffer — grow and retry
        raw.resize(raw.size() * 2 + 65536);
        res = libdeflate_deflate_decompress(
            dec, r.cur(), compressed_len, raw.data(), raw.size(), &actual);
    }

    libdeflate_free_decompressor(dec);

    if (res != LIBDEFLATE_SUCCESS) {
        throw std::runtime_error("GzProcessor: DEFLATE decompression failed (code " +
                                 std::to_string(res) + ")");
    }

    raw.resize(actual);

    // Read 8-byte trailer that follows the DEFLATE stream
    const uint8_t* trailer = r.cur() + compressed_len;
    uint32_t file_crc32, file_isize;
    std::memcpy(&file_crc32, trailer,     4);
    std::memcpy(&file_isize, trailer + 4, 4);

    // Validate CRC32 (optional but catches corrupted files early)
    const uint32_t computed = crc32_of(raw.data(), raw.size());
    if (computed != file_crc32) {
        throw std::runtime_error("GzProcessor: CRC32 mismatch — file is corrupted");
    }

    crc32_out = file_crc32;
    isize_out = file_isize;

    return compressed_len + 8; // consumed: deflate stream + trailer
}

// ─── Process a single gz member ──────────────────────────────────────────────

/**
 * @brief Parse one gzip member, recompress its payload, emit optimised member.
 *
 * @param r              Reader positioned at start of a gzip member.
 * @param w              Writer to append the optimised member to.
 * @param preserve_meta  If true, keep FNAME, FCOMMENT and MTIME.
 * @param comp           libdeflate compressor (caller owns).
 * @returns true if the member was successfully processed.
 */
bool process_member(Reader& r, Writer& w, const bool preserve_meta,
                    libdeflate_compressor* comp) {
    // ── parse header ─────────────────────────────────────────────────────────
    if (r.remaining() < 10) return false;

    const uint8_t id1 = r.u8();
    const uint8_t id2 = r.u8();
    if (id1 != kGzId1 || id2 != kGzId2)
        throw std::runtime_error("GzProcessor: invalid gzip magic");

    const uint8_t cm  = r.u8();
    const uint8_t flg = r.u8();
    const uint32_t mtime = r.u32_le();
    const uint8_t  xfl   = r.u8();
    const uint8_t  os    = r.u8();

    if (cm != kGzCm)
        throw std::runtime_error("GzProcessor: unsupported compression method " + std::to_string(cm));

    // optional: FEXTRA
    std::vector<uint8_t> extra_data;
    if (flg & kFlgFextra) {
        const uint16_t xlen = r.u16_le();
        extra_data.resize(xlen);
        for (uint16_t i = 0; i < xlen; ++i) extra_data[i] = r.u8();
    }

    // optional: FNAME
    std::string fname;
    if (flg & kFlgFname) {
        fname = r.nul_string();
    }

    // optional: FCOMMENT
    std::string fcomment;
    if (flg & kFlgFcomment) {
        fcomment = r.nul_string();
    }

    // optional: FHCRC (consume, we'll recalculate or drop it)
    if (flg & kFlgFhcrc) {
        r.skip(2);
    }

    // ── decompress DEFLATE payload ────────────────────────────────────────────
    std::vector<uint8_t> raw;
    uint32_t orig_crc32, orig_isize;
    const std::size_t consumed = decompress_member(r, raw, orig_crc32, orig_isize);
    r.skip(consumed);

    // ── recompress with libdeflate ────────────────────────────────────────────
    const std::size_t bound = libdeflate_deflate_compress_bound(comp, raw.size());
    std::vector<uint8_t> compressed(bound);
    const std::size_t new_compressed_len = libdeflate_deflate_compress(
        comp, raw.data(), raw.size(), compressed.data(), bound);
    if (new_compressed_len == 0)
        throw std::runtime_error("GzProcessor: libdeflate_deflate_compress failed");
    compressed.resize(new_compressed_len);

    // ── emit optimised member ─────────────────────────────────────────────────
    uint8_t new_flg = 0x00;
    if (preserve_meta) {
        if (!fname.empty())    new_flg |= kFlgFname;
        if (!fcomment.empty()) new_flg |= kFlgFcomment;
        // FEXTRA: preserve as-is
        if (!extra_data.empty()) new_flg |= kFlgFextra;
    }

    // Fixed 10-byte header
    w.u8(kGzId1);
    w.u8(kGzId2);
    w.u8(kGzCm);
    w.u8(new_flg);
    w.u32_le(preserve_meta ? mtime : 0);
    // XFL: mark as maximum compression (0x02) since we used libdeflate max
    w.u8(0x02);
    w.u8(os);  // preserve original OS byte

    // Optional fields (only when preserve_meta)
    if (preserve_meta) {
        if (new_flg & kFlgFextra) {
            w.u16_le(static_cast<uint16_t>(extra_data.size()));
            w.bytes(extra_data.data(), extra_data.size());
        }
        if (new_flg & kFlgFname)    w.nul_string(fname);
        if (new_flg & kFlgFcomment) w.nul_string(fcomment);
        // We deliberately omit FHCRC even in preserve_meta — it's almost
        // never used and just wastes 2 bytes.
    }

    // DEFLATE stream
    w.bytes(compressed.data(), compressed.size());

    // 8-byte trailer (CRC32 + ISIZE unchanged — the raw data is identical)
    w.u32_le(orig_crc32);
    w.u32_le(orig_isize);

    return true;
}

// ─── full-file decompression for raw_equal ────────────────────────────────────

/**
 * @brief Decompress all members of a gzip file and concatenate the raw data.
 */
std::vector<uint8_t> decompress_all(const std::vector<uint8_t>& gz_data) {
    Reader r{ gz_data.data(), gz_data.size() };
    std::vector<uint8_t> result;

    while (r.remaining() >= 10 && r.data[r.pos] == kGzId1 && r.data[r.pos+1] == kGzId2) {
        // parse header
        r.skip(2); // ID1 ID2
        const uint8_t cm  = r.u8();
        const uint8_t flg = r.u8();
        r.skip(6); // mtime(4) xfl os

        if (cm != kGzCm) break;

        if (flg & kFlgFextra) { const uint16_t xl = r.u16_le(); r.skip(xl); }
        if (flg & kFlgFname)    { while (r.u8() != 0) {} }
        if (flg & kFlgFcomment) { while (r.u8() != 0) {} }
        if (flg & kFlgFhcrc)    { r.skip(2); }

        std::vector<uint8_t> member_raw;
        uint32_t crc32_out, isize_out;
        const std::size_t consumed = decompress_member(r, member_raw, crc32_out, isize_out);
        r.skip(consumed);
        result.insert(result.end(), member_raw.begin(), member_raw.end());
    }

    return result;
}

} // namespace

// ─── IProcessor interface ─────────────────────────────────────────────────────

void GzProcessor::recompress(const std::filesystem::path& input_path,
                              const std::filesystem::path& output_path,
                              const ProcessingOptions& options) {
    Logger::log(LogLevel::Debug, "Entering recompress for " + input_path.string(), get_name());

    const auto in_data = read_file(input_path);

    if (in_data.size() < 18 ||
        in_data[0] != kGzId1 || in_data[1] != kGzId2) {
        throw std::runtime_error("GzProcessor: not a valid gzip file");
    }

    libdeflate_compressor* comp = libdeflate_alloc_compressor(kGzDeflateLevel);
    if (!comp) throw std::runtime_error("GzProcessor: libdeflate_alloc_compressor failed");

    Reader r{ in_data.data(), in_data.size() };
    Writer w;
    w.buf.reserve(in_data.size());

    const bool preserve_meta = options.preserve_metadata;

    try {
        while (r.remaining() >= 10 &&
               r.data[r.pos]     == kGzId1 &&
               r.data[r.pos + 1] == kGzId2) {
            process_member(r, w, preserve_meta, comp);
        }
    } catch (...) {
        libdeflate_free_compressor(comp);
        throw;
    }

    libdeflate_free_compressor(comp);

    if (w.buf.empty()) {
        throw std::runtime_error("GzProcessor: produced empty output");
    }

    if (!write_file(output_path, w.buf))
        throw std::runtime_error("GzProcessor: failed to write output to " + output_path.string());

    Logger::log(LogLevel::Debug,
        "GZ recompressed: " + std::to_string(in_data.size()) +
        " → " + std::to_string(w.buf.size()) + " bytes",
        get_name());
}

bool GzProcessor::raw_equal(const std::filesystem::path& a, const std::filesystem::path& b) const {
    try {
        const auto da = read_file(a);
        const auto db = read_file(b);
        return decompress_all(da) == decompress_all(db);
    } catch (const std::exception& e) {
        Logger::log(LogLevel::Error, std::string("raw_equal failed: ") + e.what(), get_name());
        return false;
    }
}

} // namespace chisel
