//
// Created by Giuseppe Francione on 20/10/26.
//

#include "../../include/zip_rewriter.hpp"
#include "../../include/archive_manifest.hpp"
#include "../../include/logger.hpp"
#include "../../include/options.hpp"
#include "../../include/thread_pool.hpp"
#include "../../include/zopfli_compressor.hpp"
#include "file_utils.hpp"
#include <archive.h>
#include <archive_entry.h>
#include <libdeflate.h>
#include <zlib.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <functional>
#include <numeric>
#include <span>
#include <stdexcept>

namespace chisel {

namespace fs = std::filesystem;

namespace {

constexpr std::uint32_t kLocalSig = 0x04034b50;
constexpr std::uint32_t kCentralSig = 0x02014b50;
constexpr std::uint32_t kEndSig = 0x06054b50;
constexpr std::uint32_t kZip64EndSig = 0x06064b50;
constexpr std::uint32_t kZip64LocatorSig = 0x07064b50;
constexpr std::uint32_t kDescriptorSig = 0x08074b50;
constexpr std::uint16_t kZip64Extra = 0x0001;
constexpr std::uint16_t kUnicodePathExtra = 0x7075;
// office's growth hint: zeros that let it rewrite a part in place, nothing a reader needs
constexpr std::uint16_t kGrowthHintExtra = 0xA220;
constexpr std::uint32_t k32 = 0xFFFFFFFFu;
constexpr std::uint16_t k16 = 0xFFFF;
constexpr std::uint16_t kStored = 0;
constexpr std::uint16_t kDeflated = 8;
// content held in memory at once while entries are compressed in parallel
constexpr std::uint64_t kBatchBytes = 256ull << 20;
constexpr std::size_t kChunk = 1 << 20;

std::uint16_t le16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | p[1] << 8); }
std::uint32_t le32(const std::uint8_t* p) { return le16(p) | static_cast<std::uint32_t>(le16(p + 2)) << 16; }
std::uint64_t le64(const std::uint8_t* p) { return le32(p) | static_cast<std::uint64_t>(le32(p + 4)) << 32; }
void put16(std::uint8_t* p, const std::uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
void put32(std::uint8_t* p, const std::uint32_t v) { put16(p, v & 0xFFFF); put16(p + 2, v >> 16); }
void put64(std::uint8_t* p, const std::uint64_t v) { put32(p, v & k32); put32(p + 4, v >> 32); }

bool read_at(std::ifstream& in, const std::uint64_t at, std::uint8_t* out, const std::size_t n) {
    in.clear();
    in.seekg(static_cast<std::streamoff>(at));
    in.read(reinterpret_cast<char*>(out), static_cast<std::streamsize>(n));
    return static_cast<std::size_t>(in.gcount()) == n;
}

/**
 * @brief Where an extra field's data starts inside a header, if the header has that field.
 */
std::optional<std::size_t> find_extra(const std::vector<std::uint8_t>& header, const std::size_t extra_at,
                                      const std::size_t length, const std::uint16_t id, std::size_t& size) {
    std::size_t at = extra_at;
    const std::size_t end = extra_at + length;
    while (at + 4 <= end) {
        const std::size_t n = le16(&header[at + 2]);
        if (at + 4 + n > end) return std::nullopt;
        if (le16(&header[at]) == id) {
            size = n;
            return at + 4;
        }
        at += 4 + n;
    }
    return std::nullopt;
}

/**
 * @brief The header without office's growth hint padding, if it has any.
 */
std::vector<std::uint8_t> without_padding(std::vector<std::uint8_t> header, const bool central) {
    const std::size_t name_at = central ? 28 : 26;
    const std::size_t extra_at = (central ? 46 : 30) + le16(&header[name_at]);
    std::size_t n = 0;
    const auto data = find_extra(header, extra_at, le16(&header[name_at + 2]), kGrowthHintExtra, n);
    if (!data) return header;
    header.erase(header.begin() + static_cast<std::ptrdiff_t>(*data - 4), header.begin() + static_cast<std::ptrdiff_t>(*data + n));
    put16(&header[name_at + 2], static_cast<std::uint16_t>(le16(&header[name_at + 2]) - 4 - n));
    return header;
}

/**
 * @brief Where a central directory record keeps its 64-bit sizes and offset, for those its 32-bit fields defer.
 */
struct Zip64Slots {
    std::optional<std::size_t> size, compressed, offset;
};

std::optional<Zip64Slots> central_zip64_slots(const std::vector<std::uint8_t>& rec) {
    Zip64Slots slots;
    const bool size = le32(&rec[24]) == k32, compressed = le32(&rec[20]) == k32, offset = le32(&rec[42]) == k32;
    if (!size && !compressed && !offset) return slots;
    std::size_t n = 0;
    const auto data = find_extra(rec, 46 + le16(&rec[28]), le16(&rec[30]), kZip64Extra, n);
    if (!data) return std::nullopt;
    std::size_t at = *data;
    const std::size_t end = *data + n;
    for (auto [wanted, slot] : {std::pair{size, &slots.size}, {compressed, &slots.compressed}, {offset, &slots.offset}}) {
        if (!wanted) continue;
        if (at + 8 > end) return std::nullopt;
        *slot = at;
        at += 8;
    }
    return slots;
}

/**
 * @brief Streams an entry's content out of its archive: stored bytes as they are, deflate inflated.
 * @return False when the data is damaged or doesn't come to the entry's size.
 */
bool stream_entry(std::ifstream& in, const ZipEntry& e, const std::function<bool(const std::uint8_t*, std::size_t)>& sink) {
    std::vector<std::uint8_t> input(std::min<std::uint64_t>(kChunk, std::max<std::uint64_t>(e.compressed, 1)));
    in.clear();
    in.seekg(static_cast<std::streamoff>(e.data_at));
    std::uint64_t left = e.compressed;
    std::uint64_t produced = 0;
    if (e.method == kStored) {
        if (e.compressed != e.size) return false;
        while (left > 0) {
            const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(left, input.size()));
            if (!read_at(in, e.data_at + (e.compressed - left), input.data(), n) || !sink(input.data(), n)) return false;
            left -= n;
        }
        return true;
    }
    z_stream z{};
    if (inflateInit2(&z, -MAX_WBITS) != Z_OK) return false;
    const struct End {
        z_stream& z;
        ~End() { inflateEnd(&z); }
    } end{z};
    std::vector<std::uint8_t> output(kChunk);
    int r = Z_OK;
    while (r != Z_STREAM_END) {
        if (z.avail_in == 0) {
            if (left == 0) return false;
            const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(left, input.size()));
            if (!read_at(in, e.data_at + (e.compressed - left), input.data(), n)) return false;
            left -= n;
            z.next_in = input.data();
            z.avail_in = static_cast<uInt>(n);
        }
        z.next_out = output.data();
        z.avail_out = static_cast<uInt>(output.size());
        r = inflate(&z, Z_NO_FLUSH);
        if (r != Z_OK && r != Z_STREAM_END) return false;
        const std::size_t got = output.size() - z.avail_out;
        produced += got;
        if (produced > e.size || (got > 0 && !sink(output.data(), got))) return false;
    }
    return produced == e.size;
}

std::uint32_t crc_of(const std::uint8_t* data, std::size_t n, std::uint32_t crc = 0) {
    while (n > 0) {
        const auto step = static_cast<uInt>(std::min<std::size_t>(n, 1u << 30));
        crc = static_cast<std::uint32_t>(crc32(crc, data, step));
        data += step;
        n -= step;
    }
    return crc;
}

bool is_utf8(const std::string& s) {
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        const std::size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (n == 0 || i + n > s.size()) return false;
        for (std::size_t k = 1; k < n; ++k) {
            if ((static_cast<unsigned char>(s[i + k]) >> 6) != 2) return false;
        }
        i += n;
    }
    return true;
}

std::string from_cp437(const std::string& s) {
    static constexpr std::array<std::uint16_t, 128> kHigh = {
        0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, 0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
        0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9, 0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
        0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA, 0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
        0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
        0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F, 0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
        0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
        0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4, 0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
        0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248, 0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0};
    std::string out;
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        const std::uint16_t u = c < 0x80 ? c : kHigh[c - 0x80];
        if (u < 0x80) {
            out += static_cast<char>(u);
        } else if (u < 0x800) {
            out += static_cast<char>(0xC0 | u >> 6);
            out += static_cast<char>(0x80 | (u & 0x3F));
        } else {
            out += static_cast<char>(0xE0 | u >> 12);
            out += static_cast<char>(0x80 | ((u >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (u & 0x3F));
        }
    }
    return out;
}

std::string raw_name(const ZipEntry& e) {
    return {reinterpret_cast<const char*>(&e.central[46]), le16(&e.central[28])};
}

// what the name says for naming the extracted file; the archive itself keeps the raw bytes
std::string display_name(const ZipEntry& e) {
    const std::string raw = raw_name(e);
    if (e.flags & 0x800) return raw;
    std::size_t n = 0;
    if (const auto at = find_extra(e.central, 46 + raw.size(), le16(&e.central[30]), kUnicodePathExtra, n);
        at && n > 5 && e.central[*at] == 1 &&
        le32(&e.central[*at + 1]) == crc_of(reinterpret_cast<const std::uint8_t*>(raw.data()), raw.size())) {
        return {reinterpret_cast<const char*>(&e.central[*at + 5]), n - 5};
    }
    return is_utf8(raw) ? raw : from_cp437(raw);
}

bool extractable(const ZipEntry& e) {
    if ((e.flags & 1) != 0 || (e.method != kStored && e.method != kDeflated)) return false;
    const std::string name = raw_name(e);
    if (name.empty() || name.back() == '/' || name.back() == '\\') return false;
    const std::uint32_t external = le32(&e.central[38]);
    const std::uint32_t unix_type = (external >> 16) & 0xF000;
    // made by unix: only regular files carry content of their own
    if (e.central[5] == 3 && unix_type != 0 && unix_type != 0x8000) return false;
    return (external & 0x10) == 0;
}

/**
 * @brief Reads where an entry's data descriptor ends, trying the layouts writers use.
 */
bool read_descriptor(std::ifstream& in, ZipEntry& e, const std::uint64_t limit) {
    const std::uint64_t at = e.data_at + e.compressed;
    std::array<std::uint8_t, 24> d{};
    const auto avail = static_cast<std::size_t>(std::min<std::uint64_t>(d.size(), limit > at ? limit - at : 0));
    if (avail < 12 || !read_at(in, at, d.data(), avail)) return false;
    const unsigned first = e.zip64_local ? 8 : 4;
    for (const bool signature : {true, false}) {
        if (signature && le32(d.data()) != kDescriptorSig) continue;
        const std::size_t o = signature ? 4 : 0;
        for (const unsigned width : {first, first == 8 ? 4u : 8u}) {
            if (o + 4 + 2 * width > avail || le32(&d[o]) != e.crc) continue;
            const std::uint64_t c = width == 8 ? le64(&d[o + 4]) : le32(&d[o + 4]);
            const std::uint64_t s = width == 8 ? le64(&d[o + 4 + width]) : le32(&d[o + 4 + width]);
            if (c != e.compressed || s != e.size) continue;
            e.descriptor = static_cast<std::uint8_t>(o + 4 + 2 * width);
            e.descriptor_signature = signature;
            return true;
        }
    }
    return false;
}

std::optional<ZipManifest> parse_directory(std::ifstream& in, const fs::path& input, const std::uint64_t file_size,
                                           const std::uint64_t end_at, const std::uint64_t archive_end) {
    std::array<std::uint8_t, 22> end{};
    if (!read_at(in, end_at, end.data(), end.size())) return std::nullopt;
    std::uint32_t disk = le16(&end[4]), cd_disk = le16(&end[6]);
    std::uint64_t on_disk = le16(&end[8]), total = le16(&end[10]), cd_size = le32(&end[12]), cd_offset = le32(&end[16]);
    const bool needs_zip64 = disk == k16 || cd_disk == k16 || on_disk == k16 || total == k16 || cd_size == k32 ||
                             cd_offset == k32;

    std::uint64_t zip64_end_at = 0;
    std::array<std::uint8_t, 20> locator{};
    if (end_at >= 20 && read_at(in, end_at - 20, locator.data(), locator.size()) && le32(locator.data()) == kZip64LocatorSig) {
        if (le32(&locator[4]) != 0 || le32(&locator[16]) > 1) return std::nullopt;
        const std::uint64_t stored = le64(&locator[8]);
        for (const std::uint64_t at : {stored, end_at >= 76 ? end_at - 76 : stored}) {
            std::array<std::uint8_t, 56> record{};
            if (at + record.size() > end_at - 20 || !read_at(in, at, record.data(), record.size()) ||
                le32(record.data()) != kZip64EndSig || at + 12 + le64(&record[4]) != end_at - 20) {
                continue;
            }
            zip64_end_at = at;
            disk = le32(&record[16]);
            cd_disk = le32(&record[20]);
            on_disk = le64(&record[24]);
            total = le64(&record[32]);
            cd_size = le64(&record[40]);
            cd_offset = le64(&record[48]);
            break;
        }
        if (zip64_end_at == 0) return std::nullopt;
    } else if (needs_zip64) {
        return std::nullopt;
    }
    // split archives can't be rewritten one file at a time
    if (disk != 0 || cd_disk != 0 || on_disk != total) return std::nullopt;

    const std::uint64_t records_end = zip64_end_at != 0 ? zip64_end_at : end_at;
    if (cd_size > records_end) return std::nullopt;
    // offsets count from the file's start, or from the archive's start when something was put in front of it
    std::vector<std::uint64_t> starts{records_end - cd_size};
    if (cd_offset + cd_size <= records_end && cd_offset != starts.front()) starts.push_back(cd_offset);
    for (const std::uint64_t cd_at : starts) {
        if (cd_at < cd_offset) continue;
        ZipManifest m;
        m.source = input;
        m.base = cd_at - cd_offset;
        m.cd_at = cd_at;
        m.cd_size = cd_size;
        m.zip64_end_at = zip64_end_at;
        m.end_at = end_at;
        m.archive_end = archive_end;
        m.file_size = file_size;

        std::vector<std::uint8_t> cd(cd_size);
        if (!read_at(in, cd_at, cd.data(), cd.size())) continue;
        bool ok = true;
        std::size_t p = 0;
        for (std::uint64_t k = 0; ok && k < total; ++k) {
            if (p + 46 > cd.size() || le32(&cd[p]) != kCentralSig) { ok = false; break; }
            const std::size_t n = le16(&cd[p + 28]);
            const std::size_t length = 46 + n + le16(&cd[p + 30]) + le16(&cd[p + 32]);
            if (p + length > cd.size()) { ok = false; break; }
            ZipEntry e;
            e.central.assign(cd.begin() + static_cast<std::ptrdiff_t>(p), cd.begin() + static_cast<std::ptrdiff_t>(p + length));
            p += length;
            e.flags = le16(&e.central[8]);
            e.method = le16(&e.central[10]);
            e.crc = le32(&e.central[16]);
            e.compressed = le32(&e.central[20]);
            e.size = le32(&e.central[24]);
            std::uint64_t offset = le32(&e.central[42]);
            const auto slots = central_zip64_slots(e.central);
            if (!slots) { ok = false; break; }
            if (slots->size) e.size = le64(&e.central[*slots->size]);
            if (slots->compressed) e.compressed = le64(&e.central[*slots->compressed]);
            if (slots->offset) offset = le64(&e.central[*slots->offset]);
            if (le16(&e.central[34]) != 0) { ok = false; break; }

            e.local_at = m.base + offset;
            std::array<std::uint8_t, 30> head{};
            if (e.local_at + 30 > cd_at || !read_at(in, e.local_at, head.data(), head.size()) || le32(head.data()) != kLocalSig) {
                ok = false;
                break;
            }
            const std::size_t local_n = le16(&head[26]), local_m = le16(&head[28]);
            e.local.resize(30 + local_n + local_m);
            if (local_n != n || !read_at(in, e.local_at, e.local.data(), e.local.size()) ||
                std::memcmp(&e.local[30], &e.central[46], n) != 0) {
                ok = false;
                break;
            }
            std::size_t z = 0;
            e.zip64_local = find_extra(e.local, 30 + n, local_m, kZip64Extra, z).has_value();
            e.data_at = e.local_at + e.local.size();
            if (e.data_at + e.compressed > cd_at || ((e.flags & 8) && !read_descriptor(in, e, cd_at))) {
                ok = false;
                break;
            }
            m.entries.push_back(std::move(e));
        }
        // anything else inside the directory, such as a digital signature record, isn't handled
        if (!ok || p != cd.size()) continue;

        m.order.resize(m.entries.size());
        std::iota(m.order.begin(), m.order.end(), std::size_t{0});
        std::ranges::sort(m.order, {}, [&m](const std::size_t i) { return m.entries[i].local_at; });
        for (std::size_t i = 0; ok && i < m.order.size(); ++i) {
            const std::uint64_t next = i + 1 < m.order.size() ? m.entries[m.order[i + 1]].local_at : cd_at;
            ok = m.entries[m.order[i]].end() <= next && m.entries[m.order[i]].local_at >= m.base;
        }
        if (ok) return m;
    }
    return std::nullopt;
}

std::optional<ZipManifest> read_layout(const fs::path& input) {
    std::ifstream in(input, std::ios::binary);
    std::error_code ec;
    const std::uint64_t size = fs::file_size(input, ec);
    if (!in || ec || size < 22) return std::nullopt;
    // the end record sits within a maximal comment of the end, give or take a footer someone appended
    const std::uint64_t window = std::min<std::uint64_t>(size, 22 + 0xFFFF + 4096);
    std::vector<std::uint8_t> tail(window);
    if (!read_at(in, size - window, tail.data(), tail.size())) return std::nullopt;
    for (std::size_t i = tail.size() - 22 + 1; i-- > 0;) {
        if (le32(&tail[i]) != kEndSig) continue;
        const std::uint64_t end_at = size - window + i;
        const std::uint64_t archive_end = end_at + 22 + le16(&tail[i + 20]);
        if (archive_end > size) continue;
        if (auto m = parse_directory(in, input, size, end_at, archive_end)) return m;
    }
    return std::nullopt;
}

/**
 * @brief The bytes and fields an entry goes back with, unless it goes back as it was.
 */
struct Payload {
    bool verbatim = true;
    std::vector<std::uint8_t> bytes;
    std::uint16_t method = kStored;
    std::uint32_t crc = 0;
    std::uint64_t size = 0;
};

std::vector<std::uint8_t> deflate_libdeflate(const std::vector<std::uint8_t>& content, const int level) {
    const std::unique_ptr<libdeflate_compressor, decltype(&libdeflate_free_compressor)> c(
        libdeflate_alloc_compressor(level), libdeflate_free_compressor);
    if (!c) throw std::runtime_error("libdeflate_alloc_compressor failed");
    std::vector<std::uint8_t> out(libdeflate_deflate_compress_bound(c.get(), content.size()));
    const std::size_t n = libdeflate_deflate_compress(c.get(), content.data(), content.size(), out.data(), out.size());
    if (n == 0) throw std::runtime_error("libdeflate_deflate_compress failed");
    out.resize(n);
    return out;
}

// zlib at its best settings, what libarchive's zip writer used, so no entry comes out bigger than it did
std::vector<std::uint8_t> deflate_zlib(const std::vector<std::uint8_t>& content) {
    z_stream z{};
    if (deflateInit2(&z, 9, Z_DEFLATED, -MAX_WBITS, 9, Z_DEFAULT_STRATEGY) != Z_OK) throw std::runtime_error("deflateInit2 failed");
    const struct End {
        z_stream& z;
        ~End() { deflateEnd(&z); }
    } end{z};
    std::vector<std::uint8_t> out(deflateBound(&z, static_cast<uLong>(content.size())));
    z.next_in = const_cast<Bytef*>(content.data());
    z.avail_in = static_cast<uInt>(content.size());
    z.next_out = out.data();
    z.avail_out = static_cast<uInt>(out.size());
    if (deflate(&z, Z_FINISH) != Z_STREAM_END) throw std::runtime_error("deflate failed");
    out.resize(z.total_out);
    return out;
}

bool same_as_source(const ZipManifest& m, const ZipEntry& e, const std::vector<std::uint8_t>& content) {
    std::ifstream in(m.source, std::ios::binary);
    std::size_t at = 0;
    return in && stream_entry(in, e, [&](const std::uint8_t* data, const std::size_t n) {
        if (at + n > content.size() || std::memcmp(content.data() + at, data, n) != 0) return false;
        at += n;
        return true;
    }) && at == content.size();
}

Payload build_payload(const ZipManifest& m, const ZipEntry& e, const bool first, const ProcessingOptions& options,
                      const ZipWriteOptions& write_options) {
    Payload p;
    if (e.data.empty()) return p;
    std::vector<std::uint8_t> content;
    if (!read_file(e.data, content)) throw std::runtime_error("can't read " + e.data.string());
    const std::uint32_t crc = crc_of(content.data(), content.size());
    const bool same = content.size() == e.size && crc == e.crc && same_as_source(m, e, content);
    // a leading "mimetype" is how epub, odf and openraster readers recognize the file, and it must stay stored
    const bool may_deflate = e.method == kDeflated || (write_options.deflate_stored && !(first && raw_name(e) == "mimetype"));
    const std::uint64_t kept = e.method == kDeflated ? e.compressed : e.size;
    if (!may_deflate || content.empty()) {
        if (same) return p;
        p.verbatim = false;
        p.method = e.method;
        p.crc = crc;
        p.size = content.size();
        p.bytes = std::move(content);
        return p;
    }

    // content a quick pass barely shrinks, like a comic book's jpegs, gains ~0.3% from the slow passes at many times the cost
    std::vector<std::uint8_t> best = deflate_zlib(content);
    if (static_cast<std::uint64_t>(best.size()) * 100 <= static_cast<std::uint64_t>(content.size()) * 90) {
        if (auto better = deflate_libdeflate(content, 12); better.size() < best.size()) best = std::move(better);
        const unsigned iterations = static_cast<unsigned>(content.size() < 200000 ? options.iterations : options.iterations_large);
        if (auto z = ZopfliCompressor::compress(content, iterations, ZopfliFormat::DEFLATE); !z.empty() && z.size() < best.size()) {
            best = std::move(z);
        }
    }
    if (same && kept <= best.size()) return p;
    p.verbatim = false;
    p.crc = crc;
    p.size = content.size();
    if (e.method == kStored && content.size() <= best.size()) {
        p.method = kStored;
        p.bytes = std::move(content);
    } else {
        p.method = kDeflated;
        p.bytes = std::move(best);
    }
    return p;
}

/**
 * @brief An entry's fields as written, for its central directory record.
 */
struct Written {
    std::uint64_t offset = 0; ///< local header offset, as stored
    bool rewritten = false;
    std::uint16_t method = 0;
    std::uint32_t crc = 0;
    std::uint64_t compressed = 0;
    std::uint64_t size = 0;
};

bool set_method(std::vector<std::uint8_t>& header, const std::size_t method_at, const std::size_t version_at,
                const std::uint16_t method) {
    if (le16(&header[method_at]) == method) return true;
    put16(&header[method_at], method);
    // deflate needs 2.0 to extract
    if (method == kDeflated && (le16(&header[version_at]) & 0xFF) < 20) {
        put16(&header[version_at], static_cast<std::uint16_t>((le16(&header[version_at]) & 0xFF00) | 20));
    }
    return true;
}

bool patch_local(const ZipEntry& e, const Written& w, std::vector<std::uint8_t>& local) {
    local = without_padding(e.local, false);
    set_method(local, 8, 4, w.method);
    // with a data descriptor the local header may leave all three zero, and then it stays that way
    if ((e.flags & 8) && le32(&local[14]) == 0 && le32(&local[18]) == 0 && le32(&local[22]) == 0) return true;
    put32(&local[14], w.crc);
    std::size_t n = 0;
    const auto z = find_extra(local, 30 + le16(&local[26]), le16(&local[28]), kZip64Extra, n);
    for (auto [at, extra_at, value] : {std::tuple{std::size_t{22}, std::size_t{0}, w.size}, {18, 8, w.compressed}}) {
        if (le32(&local[at]) == k32) {
            if (!z || extra_at + 8 > n) return false;
            put64(&local[*z + extra_at], value);
        } else {
            if (value >= k32) return false;
            put32(&local[at], static_cast<std::uint32_t>(value));
            if (z && extra_at + 8 <= n) put64(&local[*z + extra_at], value);
        }
    }
    return true;
}

bool patch_central(const ZipEntry& e, const Written& w, std::vector<std::uint8_t>& rec) {
    rec = without_padding(e.central, true);
    const auto slots = central_zip64_slots(rec);
    if (!slots) return false;
    const auto put = [&rec](const std::size_t at, const std::optional<std::size_t> slot, const std::uint64_t value) {
        if (slot) {
            put64(&rec[*slot], value);
            return true;
        }
        if (value >= k32) return false;
        put32(&rec[at], static_cast<std::uint32_t>(value));
        return true;
    };
    if (!put(42, slots->offset, w.offset)) return false;
    if (!w.rewritten) return true;
    set_method(rec, 10, 6, w.method);
    put32(&rec[16], w.crc);
    return put(24, slots->size, w.size) && put(20, slots->compressed, w.compressed);
}

bool copy_range(std::ifstream& in, std::ofstream& out, const std::uint64_t from, const std::uint64_t to,
                std::vector<char>& buffer) {
    in.clear();
    in.seekg(static_cast<std::streamoff>(from));
    for (std::uint64_t left = to - from; left > 0;) {
        const auto n = static_cast<std::streamsize>(std::min<std::uint64_t>(left, buffer.size()));
        if (!in.read(buffer.data(), n)) return false;
        out.write(buffer.data(), n);
        left -= static_cast<std::uint64_t>(n);
    }
    return static_cast<bool>(out);
}

bool same_range(std::ifstream& a, const std::uint64_t a_at, std::ifstream& b, const std::uint64_t b_at, std::uint64_t n) {
    std::vector<std::uint8_t> x(kChunk), y(kChunk);
    for (std::uint64_t done = 0; done < n;) {
        const auto step = static_cast<std::size_t>(std::min<std::uint64_t>(n - done, kChunk));
        if (!read_at(a, a_at + done, x.data(), step) || !read_at(b, b_at + done, y.data(), step) ||
            std::memcmp(x.data(), y.data(), step) != 0) {
            return false;
        }
        done += step;
    }
    return true;
}

// the header with every field a rewrite may change zeroed, so that the rest can be compared
std::vector<std::uint8_t> masked(std::vector<std::uint8_t> header, const bool central) {
    const std::size_t name_at = central ? 28 : 26;
    const std::size_t fixed = central ? 46 : 30;
    std::size_t n = 0;
    if (const auto z = find_extra(header, fixed + le16(&header[name_at]), le16(&header[name_at + 2]), kZip64Extra, n)) {
        std::fill_n(header.begin() + static_cast<std::ptrdiff_t>(*z), n, 0);
    }
    const auto clear = [&header](const std::size_t at, const std::size_t count) {
        std::fill_n(header.begin() + static_cast<std::ptrdiff_t>(at), count, 0);
    };
    if (central) {
        clear(6, 2);
        clear(10, 2);
        clear(16, 12);
        clear(42, 4);
    } else {
        clear(4, 2);
        clear(8, 2);
        clear(14, 12);
    }
    return header;
}

} // namespace

std::optional<ZipManifest> read_zip_manifest(const fs::path& input, const fs::path& dest_dir, const std::string_view tag) {
    auto m = read_layout(input);
    if (!m) return std::nullopt;
    std::ifstream in(input, std::ios::binary);
    if (!in) return std::nullopt;
    for (const std::size_t index : m->order) {
        ZipEntry& e = m->entries[index];
        if (!extractable(e)) continue;
        const fs::path out = entry_extraction_path(display_name(e), dest_dir, index);
        if (out.empty()) continue;
        std::ofstream os(out, std::ios::binary);
        std::uint32_t crc = 0;
        const bool ok = os && stream_entry(in, e, [&](const std::uint8_t* data, const std::size_t n) {
            crc = crc_of(data, n, crc);
            os.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(n));
            return static_cast<bool>(os);
        });
        os.close();
        if (!ok || !os || crc != e.crc) {
            std::error_code ec;
            fs::remove(out, ec);
            Logger::log(LogLevel::Warning, "Entry " + display_name(e) + " doesn't match its CRC, kept as it is", tag);
            continue;
        }
        e.data = out;
    }
    return m;
}

std::vector<fs::path> zip_manifest_files(const ZipManifest& manifest) {
    std::vector<fs::path> files;
    for (const std::size_t index : manifest.order) {
        if (!manifest.entries[index].data.empty()) files.push_back(manifest.entries[index].data);
    }
    return files;
}

bool write_zip_manifest(const ZipManifest& m, const fs::path& output, const ProcessingOptions& options,
                        const ZipWriteOptions& write_options, const std::string_view tag) {
    // a write that doesn't finish leaves nothing behind; declared first, it runs after the stream closes
    struct PartialGuard {
        const fs::path& path;
        bool written = false;
        ~PartialGuard() {
            std::error_code ec;
            if (!written) fs::remove(path, ec);
        }
    } partial{output};
    std::ifstream in(m.source, std::ios::binary);
    std::ofstream out(output, std::ios::binary | std::ios::trunc);
    if (!in || !out) {
        Logger::log(LogLevel::Error, "Can't open " + (in ? output : m.source).string(), tag);
        return false;
    }
    std::vector<char> buffer(kChunk);
    std::vector<Written> written(m.entries.size());
    const std::size_t first = m.order.empty() ? 0 : m.order.front();
    std::uint64_t at = 0;
    std::uint64_t copied_to = 0;

    for (std::size_t from = 0; from < m.order.size();) {
        // a batch of entries, in file order, whose content fits in memory at once
        std::size_t to = from;
        std::uint64_t bytes = 0;
        while (to < m.order.size() && (to == from || bytes < kBatchBytes)) {
            const ZipEntry& e = m.entries[m.order[to++]];
            if (!e.data.empty()) bytes += e.size;
        }
        std::vector<Payload> payloads(to - from);
        std::vector<std::size_t> biggest(to - from);
        std::iota(biggest.begin(), biggest.end(), std::size_t{0});
        std::ranges::stable_sort(biggest, std::ranges::greater{}, [&](const std::size_t i) { return m.entries[m.order[from + i]].size; });
        const auto build = [&](const std::size_t i) {
            const std::size_t index = m.order[from + biggest[i]];
            payloads[biggest[i]] = build_payload(m, m.entries[index], index == first, options, write_options);
        };
        if (ThreadPool* pool = ThreadPool::current()) {
            pool->parallel_for(payloads.size(), build);
        } else {
            for (std::size_t i = 0; i < payloads.size(); ++i) build(i);
        }

        for (std::size_t i = 0; i < payloads.size(); ++i) {
            const std::size_t index = m.order[from + i];
            const ZipEntry& e = m.entries[index];
            Payload& p = payloads[i];
            if (!copy_range(in, out, copied_to, e.local_at, buffer)) return false;
            at += e.local_at - copied_to;
            Written& w = written[index];
            w.offset = at - m.base;
            if (p.verbatim) {
                const std::vector<std::uint8_t> local = without_padding(e.local, false);
                out.write(reinterpret_cast<const char*>(local.data()), static_cast<std::streamsize>(local.size()));
                if (!copy_range(in, out, e.data_at, e.end(), buffer)) return false;
                at += local.size() + (e.end() - e.data_at);
            } else {
                w = {.offset = at - m.base, .rewritten = true, .method = p.method, .crc = p.crc,
                     .compressed = p.bytes.size(), .size = p.size};
                std::vector<std::uint8_t> local;
                if (!patch_local(e, w, local)) {
                    Logger::log(LogLevel::Error, "Entry " + display_name(e) + " outgrew its header fields", tag);
                    return false;
                }
                out.write(reinterpret_cast<const char*>(local.data()), static_cast<std::streamsize>(local.size()));
                out.write(reinterpret_cast<const char*>(p.bytes.data()), static_cast<std::streamsize>(p.bytes.size()));
                at += local.size() + p.bytes.size();
                if (e.descriptor > 0) {
                    std::array<std::uint8_t, 24> d{};
                    std::size_t o = 0;
                    if (e.descriptor_signature) {
                        put32(d.data(), kDescriptorSig);
                        o = 4;
                    }
                    put32(&d[o], w.crc);
                    if (e.descriptor - o == 20) {
                        put64(&d[o + 4], w.compressed);
                        put64(&d[o + 12], w.size);
                    } else {
                        if (w.compressed >= k32 || w.size >= k32) return false;
                        put32(&d[o + 4], static_cast<std::uint32_t>(w.compressed));
                        put32(&d[o + 8], static_cast<std::uint32_t>(w.size));
                    }
                    out.write(reinterpret_cast<const char*>(d.data()), e.descriptor);
                    at += e.descriptor;
                }
            }
            copied_to = e.end();
            p = {};
        }
        from = to;
    }

    if (!copy_range(in, out, copied_to, m.cd_at, buffer)) return false;
    at += m.cd_at - copied_to;
    const std::uint64_t new_cd_at = at;
    for (std::size_t index = 0; index < m.entries.size(); ++index) {
        std::vector<std::uint8_t> rec;
        if (!patch_central(m.entries[index], written[index], rec)) {
            Logger::log(LogLevel::Error, "Entry " + display_name(m.entries[index]) + " outgrew its directory fields", tag);
            return false;
        }
        out.write(reinterpret_cast<const char*>(rec.data()), static_cast<std::streamsize>(rec.size()));
    }

    const std::uint64_t new_cd_size = out.tellp() - static_cast<std::streamoff>(new_cd_at);
    // the zip64 end records and the end record, with the directory's new place and size
    std::vector<std::uint8_t> tail(m.archive_end - (m.cd_at + m.cd_size));
    if (!read_at(in, m.cd_at + m.cd_size, tail.data(), tail.size())) return false;
    const auto in_tail = [&m](const std::uint64_t absolute) { return absolute - (m.cd_at + m.cd_size); };
    const std::uint64_t cd_offset = new_cd_at - m.base;
    if (m.zip64_end_at != 0) {
        put64(&tail[in_tail(m.zip64_end_at) + 40], new_cd_size);
        put64(&tail[in_tail(m.zip64_end_at) + 48], cd_offset);
        put64(&tail[in_tail(m.end_at - 20) + 8], m.zip64_end_at + (new_cd_at - m.cd_at) - m.base);
    }
    if (le32(&tail[in_tail(m.end_at) + 12]) != k32) put32(&tail[in_tail(m.end_at) + 12], static_cast<std::uint32_t>(new_cd_size));
    if (le32(&tail[in_tail(m.end_at) + 16]) != k32) {
        if (cd_offset >= k32) return false;
        put32(&tail[in_tail(m.end_at) + 16], static_cast<std::uint32_t>(cd_offset));
    }
    out.write(reinterpret_cast<const char*>(tail.data()), static_cast<std::streamsize>(tail.size()));
    if (write_options.keep_trailing && !copy_range(in, out, m.archive_end, m.file_size, buffer)) return false;
    out.close();
    if (!out) {
        Logger::log(LogLevel::Error, "Can't write " + output.string(), tag);
        return false;
    }
    partial.written = true;
    return true;
}

std::string compare_zip_manifest(const fs::path& rebuilt, const ZipManifest& m) {
    const auto r = read_layout(rebuilt);
    if (!r) return "does not read back as a zip";
    if (r->entries.size() != m.entries.size()) return "different number of entries";
    if (r->archive_end - r->end_at != m.archive_end - m.end_at) return "different archive comment";
    std::ifstream source(m.source, std::ios::binary);
    std::ifstream copy(rebuilt, std::ios::binary);
    if (!source || !copy) return "can't open";
    if (!same_range(source, m.end_at + 20, copy, r->end_at + 20, m.archive_end - m.end_at - 20)) {
        return "different archive comment";
    }
    bool plain = true;
    for (std::size_t i = 0; i < m.entries.size(); ++i) {
        const ZipEntry& a = m.entries[i];
        const ZipEntry& b = r->entries[i];
        const std::string name = display_name(a);
        if (masked(without_padding(a.central, true), true) != masked(b.central, true)) return name + ": different directory record";
        if (masked(without_padding(a.local, false), false) != masked(b.local, false)) return name + ": different local header";
        if (a.descriptor != b.descriptor || a.descriptor_signature != b.descriptor_signature) return name + ": different data descriptor";
        plain = plain && (a.flags & 1) == 0 && (a.method == kStored || a.method == kDeflated);
        if (a.data.empty()) {
            if (b.end() - b.data_at != a.end() - a.data_at || !same_range(source, a.data_at, copy, b.data_at, a.end() - a.data_at)) {
                return name + ": changed although it was to be copied";
            }
            continue;
        }
        std::ifstream file(a.data, std::ios::binary);
        std::vector<std::uint8_t> want(kChunk);
        std::uint64_t seen = 0;
        std::uint32_t crc = 0;
        const bool same = file && stream_entry(copy, b, [&](const std::uint8_t* data, const std::size_t n) {
            file.read(reinterpret_cast<char*>(want.data()), static_cast<std::streamsize>(n));
            crc = crc_of(data, n, crc);
            seen += n;
            return static_cast<std::size_t>(file.gcount()) == n && std::memcmp(want.data(), data, n) == 0;
        });
        if (!same || file.peek() != std::ifstream::traits_type::eof() || crc != b.crc || seen != b.size) {
            return name + ": different data";
        }
    }
    for (std::size_t i = 0; i < m.order.size(); ++i) {
        if (m.order[i] != r->order[i]) return "entries in a different order";
    }

    // an independent reader must take it too
    const std::unique_ptr<archive, decltype(&archive_read_free)> a(archive_read_new(), archive_read_free);
    if (!a) return "no reader";
    archive_read_support_format_zip(a.get());
    archive_read_set_options(a.get(), "zip:!mac-ext");
#ifdef _WIN32
    if (archive_read_open_filename_w(a.get(), rebuilt.wstring().c_str(), 10240) < ARCHIVE_WARN) {
#else
    if (archive_read_open_filename(a.get(), rebuilt.string().c_str(), 10240) < ARCHIVE_WARN) {
#endif
        return "libarchive can't open it: " + archive_error(a.get());
    }
    archive_entry* entry = nullptr;
    std::size_t count = 0;
    std::vector<char> sink(kChunk);
    int status = ARCHIVE_OK;
    while ((status = archive_read_next_header(a.get(), &entry)) != ARCHIVE_EOF) {
        if (status < ARCHIVE_WARN) return "libarchive: " + archive_error(a.get());
        ++count;
        // other methods and encrypted entries were copied as they were, and libarchive may not read them
        if (!plain) continue;
        la_ssize_t n = 0;
        while ((n = archive_read_data(a.get(), sink.data(), sink.size())) > 0) {}
        if (n < 0) return "libarchive: " + archive_error(a.get());
    }
    if (count != m.entries.size()) return "libarchive sees " + std::to_string(count) + " entries";
    return {};
}

void rewrite_zip(const ZipManifest& manifest, const fs::path& output, const ProcessingOptions& options,
                 const ZipWriteOptions& write_options, const std::string_view tag) {
    if (!write_zip_manifest(manifest, output, options, write_options, tag)) {
        throw std::runtime_error(std::string(tag) + ": writing " + output.filename().string() + " failed");
    }
    const auto start = std::chrono::steady_clock::now();
    // only a zip that reads back as its manifest may replace the original
    if (const std::string diff = compare_zip_manifest(output, manifest); !diff.empty()) {
        std::error_code ec;
        fs::remove(output, ec);
        throw std::runtime_error(std::string(tag) + ": the rebuilt zip differs from its source at " + diff);
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    Logger::log(LogLevel::Debug, "Rebuild check of " + output.string() + " took " + std::to_string(ms.count()) + " ms", tag);
}

} // namespace chisel
