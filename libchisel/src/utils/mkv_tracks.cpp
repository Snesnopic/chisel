//
// Created by Giuseppe Francione on 21/10/26.
//

#include "../../include/mkv_tracks.hpp"
#include <libdeflate.h>
#include <zlib.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <vector>

namespace chisel {

namespace fs = std::filesystem;

namespace {

constexpr std::uint32_t kEbmlHeader = 0x1A45DFA3;
constexpr std::uint32_t kSegment = 0x18538067;
constexpr std::uint32_t kSeekHead = 0x114D9B74;
constexpr std::uint32_t kInfo = 0x1549A966;
constexpr std::uint32_t kTracks = 0x1654AE6B;
constexpr std::uint32_t kCluster = 0x1F43B675;
constexpr std::uint32_t kCues = 0x1C53BB6B;
constexpr std::uint32_t kAttachments = 0x1941A469;
constexpr std::uint32_t kChapters = 0x1043A770;
constexpr std::uint32_t kTags = 0x1254C367;
constexpr std::uint32_t kTrackEntry = 0xAE;
constexpr std::uint32_t kTrackNumber = 0xD7;
constexpr std::uint32_t kTrackUid = 0x73C5;
constexpr std::uint32_t kCodecId = 0x86;
constexpr std::uint32_t kCodecPrivate = 0x63A2;
constexpr std::uint32_t kContentEncodings = 0x6D80;
constexpr std::uint32_t kContentEncoding = 0x6240;
constexpr std::uint32_t kEncodingScope = 0x5032;
constexpr std::uint32_t kEncodingType = 0x5033;
constexpr std::uint32_t kCompression = 0x5034;
constexpr std::uint32_t kEncryption = 0x5035;
constexpr std::uint32_t kCompAlgo = 0x4254;
constexpr std::uint32_t kCompSettings = 0x4255;
constexpr std::uint32_t kSimpleBlock = 0xA3;
constexpr std::uint32_t kBlockGroup = 0xA0;
constexpr std::uint32_t kBlock = 0xA1;
constexpr std::uint32_t kEncryptedBlock = 0xAF;
constexpr std::uint32_t kBlockAdditions = 0x75A1;
constexpr std::uint32_t kBlockMore = 0xA6;
constexpr std::uint32_t kBlockAddId = 0xEE;
constexpr std::uint32_t kBlockAdditional = 0xA5;
constexpr std::uint32_t kDiscardPadding = 0x75A2;
constexpr std::uint32_t kSeek = 0x4DBB;
constexpr std::uint32_t kSeekId = 0x53AB;
constexpr std::uint32_t kSeekPosition = 0x53AC;
constexpr std::uint32_t kCuePoint = 0xBB;
constexpr std::uint32_t kCueTrackPositions = 0xB7;
constexpr std::uint32_t kCueTrack = 0xF7;
constexpr std::uint32_t kCueClusterPosition = 0xF1;
constexpr std::uint32_t kCueRelativePosition = 0xF0;
constexpr std::uint64_t kUnknownSize = ~0ull;
constexpr std::uint64_t kMaxElement = 1ull << 30;  ///< largest element or frame held in memory at once

bool is_level1(const std::uint32_t id) {
    switch (id) {
        case kSeekHead: case kInfo: case kTracks: case kCluster: case kCues:
        case kAttachments: case kChapters: case kTags:
            return true;
        default:
            return false;
    }
}

/**
 * @brief Reads an element id (marker bits kept) and data size from p[at..len), moving at past them.
 */
bool parse_header(const std::uint8_t* p, const std::size_t len, std::size_t& at, std::uint32_t& id, std::uint64_t& size) {
    if (at >= len || p[at] == 0) return false;
    unsigned n = 1;
    while (n <= 4 && !(p[at] & (0x80 >> (n - 1)))) ++n;
    if (n > 4 || at + n > len) return false;
    id = 0;
    for (unsigned i = 0; i < n; ++i) id = id << 8 | p[at + i];
    at += n;
    if (at >= len || p[at] == 0) return false;
    unsigned m = 1;
    while (m <= 8 && !(p[at] & (0x80 >> (m - 1)))) ++m;
    if (m > 8 || at + m > len) return false;
    std::uint64_t v = p[at] & (0xFF >> m);
    bool all_ones = v == (0xFFu >> m);
    for (unsigned i = 1; i < m; ++i) {
        v = v << 8 | p[at + i];
        all_ones = all_ones && p[at + i] == 0xFF;
    }
    at += m;
    size = all_ones ? kUnknownSize : v;
    return true;
}

std::uint64_t unsigned_value(const std::uint8_t* p, const std::uint64_t n) {
    std::uint64_t v = 0;
    for (std::uint64_t i = 0; i < n && i < 8; ++i) v = v << 8 | p[i];
    return v;
}

std::int64_t signed_value(const std::uint8_t* p, const std::uint64_t n) {
    if (n == 0 || n > 8) return 0;
    const std::uint64_t v = unsigned_value(p, n);
    const unsigned unused = 64 - 8 * static_cast<unsigned>(n);
    return unused == 0 ? static_cast<std::int64_t>(v) : static_cast<std::int64_t>(v << unused) >> unused;
}

/**
 * @brief Calls visit(id, data, size) for every child of the master element held in p[0..len).
 */
template <typename Visit>
bool for_children(const std::uint8_t* p, const std::size_t len, Visit visit) {
    std::size_t at = 0;
    while (at < len) {
        std::uint32_t id = 0;
        std::uint64_t size = 0;
        if (!parse_header(p, len, at, id, size) || size == kUnknownSize || size > len - at) return false;
        if (!visit(id, p + at, static_cast<std::size_t>(size))) return false;
        at += static_cast<std::size_t>(size);
    }
    return true;
}

std::optional<std::vector<std::uint8_t>> inflate_all(const std::uint8_t* p, const std::size_t n) {
    z_stream z{};
    if (inflateInit(&z) != Z_OK) return std::nullopt;
    std::vector<std::uint8_t> out(std::min<std::size_t>(n, kMaxElement / 4) * 4 + 64);
    z.next_in = const_cast<Bytef*>(p);
    z.avail_in = static_cast<uInt>(n);
    int r = Z_OK;
    while (r == Z_OK) {
        if (z.total_out == out.size()) {
            if (out.size() >= kMaxElement) break;
            out.resize(out.size() * 2);
        }
        z.next_out = out.data() + z.total_out;
        z.avail_out = static_cast<uInt>(out.size() - z.total_out);
        r = inflate(&z, Z_NO_FLUSH);
        if (r == Z_BUF_ERROR && z.avail_out == 0) r = Z_OK;
    }
    const auto total = z.total_out;
    inflateEnd(&z);
    if (r != Z_STREAM_END) return std::nullopt;
    out.resize(total);
    return out;
}

struct Digest {
    std::uint64_t bytes = 0;
    std::uint32_t crc = 0;
    std::uint32_t adler = 1;

    void add(const std::uint8_t* p, const std::size_t n) {
        // libdeflate takes a null buffer as a request for the initial value
        if (n == 0) return;
        crc = libdeflate_crc32(crc, p, n);
        adler = libdeflate_adler32(adler, p, n);
        bytes += n;
    }
    void add_number(const std::uint64_t v) {
        std::array<std::uint8_t, 8> b{};
        for (unsigned i = 0; i < 8; ++i) b[i] = static_cast<std::uint8_t>(v >> (8 * i));
        add(b.data(), b.size());
    }
    bool operator==(const Digest&) const = default;
};

/**
 * @brief A track as declared, and a digest of every frame it holds, in order.
 */
struct Track {
    std::uint64_t uid = 0;
    std::string codec;
    std::vector<std::uint8_t> codec_private;
    std::vector<std::uint8_t> header;  ///< header removal bytes put back in front of every frame
    bool zlib_frames = false;
    std::uint64_t frames = 0;
    std::uint64_t bytes = 0;
    Digest data;                       ///< every frame, each preceded by its size
    Digest side;                       ///< block additions and discard padding, in block order
};

/**
 * @brief Where the file's own index says an element is: a SeekHead entry or a cue.
 */
struct IndexEntry {
    std::uint64_t position = 0;          ///< relative to the segment data
    std::uint32_t id = 0;                ///< what has to be there
    std::optional<std::uint64_t> block;  ///< for a cue, its block relative to the cluster data
    std::uint64_t track = 0;             ///< for a cue, the track it's for
};

struct Tracks {
    std::map<std::uint64_t, Track> by_number;
    std::vector<IndexEntry> index;
    std::string error;
};

bool parse_track_entry(const std::uint8_t* p, const std::size_t len, Tracks& tracks) {
    std::uint64_t number = 0;
    Track track;
    bool private_zlib = false;
    int encodings = 0;
    bool ok = for_children(p, len, [&](const std::uint32_t id, const std::uint8_t* d, const std::size_t n) {
        if (id == kTrackNumber) number = unsigned_value(d, n);
        else if (id == kTrackUid) track.uid = unsigned_value(d, n);
        else if (id == kCodecId) track.codec.assign(reinterpret_cast<const char*>(d), strnlen(reinterpret_cast<const char*>(d), n));
        else if (id == kCodecPrivate) track.codec_private.assign(d, d + n);
        else if (id == kContentEncodings) {
            return for_children(d, n, [&](const std::uint32_t eid, const std::uint8_t* e, const std::size_t en) {
                if (eid != kContentEncoding) return true;
                ++encodings;
                std::uint64_t scope = 1, type = 0, algo = 0;
                std::vector<std::uint8_t> settings;
                bool compressed = false, encrypted = false;
                const bool parsed = for_children(e, en, [&](const std::uint32_t cid, const std::uint8_t* c, const std::size_t cn) {
                    if (cid == kEncodingScope) scope = unsigned_value(c, cn);
                    else if (cid == kEncodingType) type = unsigned_value(c, cn);
                    else if (cid == kEncryption) encrypted = true;
                    else if (cid == kCompression) {
                        compressed = true;
                        return for_children(c, cn, [&](const std::uint32_t aid, const std::uint8_t* a, const std::size_t an) {
                            if (aid == kCompAlgo) algo = unsigned_value(a, an);
                            else if (aid == kCompSettings) settings.assign(a, a + an);
                            return true;
                        });
                    }
                    return true;
                });
                if (!parsed || encrypted || type != 0 || !compressed) {
                    tracks.error = "encrypted or unknown track encoding";
                    return false;
                }
                if (algo == 3) {
                    if (scope & 1) track.header = settings;
                } else if (algo == 0) {
                    track.zlib_frames = (scope & 1) != 0;
                    private_zlib = (scope & 2) != 0;
                } else {
                    tracks.error = "bzlib or lzo compressed track";
                    return false;
                }
                return true;
            });
        }
        return true;
    });
    if (!ok) {
        if (tracks.error.empty()) tracks.error = "unreadable track entry";
        return false;
    }
    if (encodings > 1) {
        tracks.error = "cascaded track encodings";
        return false;
    }
    if (private_zlib && !track.codec_private.empty()) {
        auto plain = inflate_all(track.codec_private.data(), track.codec_private.size());
        if (!plain) {
            tracks.error = "undecodable codec private data";
            return false;
        }
        track.codec_private = std::move(*plain);
    }
    if (number == 0) {
        tracks.error = "track without a number";
        return false;
    }
    tracks.by_number[number] = std::move(track);
    return true;
}

bool read_vint(const std::uint8_t* p, const std::size_t len, std::size_t& at, std::uint64_t& value, unsigned& length) {
    if (at >= len || p[at] == 0) return false;
    length = 1;
    while (length <= 8 && !(p[at] & (0x80 >> (length - 1)))) ++length;
    if (length > 8 || at + length > len) return false;
    value = p[at] & (0xFF >> length);
    for (unsigned i = 1; i < length; ++i) value = value << 8 | p[at + i];
    at += length;
    return true;
}

/**
 * @brief Splits a Block or SimpleBlock into its frames and adds them to their track's digest.
 */
bool add_block(const std::uint8_t* p, const std::size_t len, Tracks& tracks, std::uint64_t& number) {
    std::size_t at = 0;
    unsigned vlen = 0;
    if (!read_vint(p, len, at, number, vlen) || at + 3 > len) return false;
    const std::uint8_t flags = p[at + 2];
    at += 3;
    auto it = tracks.by_number.find(number);
    if (it == tracks.by_number.end()) {
        tracks.error = "block of undeclared track " + std::to_string(number);
        return false;
    }
    Track& track = it->second;

    std::vector<std::size_t> sizes;
    const unsigned lacing = (flags >> 1) & 3;
    if (lacing == 0) {
        sizes.push_back(len - at);
    } else {
        if (at >= len) return false;
        const std::size_t count = p[at++] + 1u;
        std::size_t used = 0;
        if (lacing == 1) {
            for (std::size_t i = 0; i + 1 < count; ++i) {
                std::size_t s = 0;
                while (true) {
                    if (at >= len) return false;
                    const std::uint8_t b = p[at++];
                    s += b;
                    if (b != 0xFF) break;
                }
                sizes.push_back(s);
                used += s;
                if (used > len) return false;
            }
        } else if (lacing == 3) {
            std::uint64_t first = 0;
            if (!read_vint(p, len, at, first, vlen) || first > len) return false;
            sizes.push_back(static_cast<std::size_t>(first));
            used = static_cast<std::size_t>(first);
            std::int64_t previous = static_cast<std::int64_t>(first);
            for (std::size_t i = 1; i + 1 < count; ++i) {
                std::uint64_t raw = 0;
                if (!read_vint(p, len, at, raw, vlen)) return false;
                const std::int64_t delta = static_cast<std::int64_t>(raw) - ((std::int64_t{1} << (7 * vlen - 1)) - 1);
                previous += delta;
                if (previous < 0 || static_cast<std::uint64_t>(previous) > len) return false;
                sizes.push_back(static_cast<std::size_t>(previous));
                used += static_cast<std::size_t>(previous);
                if (used > len) return false;
            }
        } else {
            if ((len - at) % count != 0) return false;
            sizes.assign(count - 1, (len - at) / count);
            used = (len - at) / count * (count - 1);
        }
        if (used > len - at) return false;
        sizes.push_back(len - at - used);
    }

    for (const std::size_t size : sizes) {
        if (size > len - at) return false;
        const std::uint8_t* frame = p + at;
        at += size;
        std::vector<std::uint8_t> plain;
        if (track.zlib_frames) {
            auto inflated = inflate_all(frame, size);
            if (!inflated) {
                tracks.error = "undecodable zlib frame in track " + std::to_string(number);
                return false;
            }
            plain = std::move(*inflated);
        }
        const std::uint8_t* data = track.zlib_frames ? plain.data() : frame;
        const std::size_t data_size = track.zlib_frames ? plain.size() : size;
        track.data.add_number(track.header.size() + data_size);
        track.data.add(track.header.data(), track.header.size());
        track.data.add(data, data_size);
        track.bytes += track.header.size() + data_size;
        ++track.frames;
    }
    return true;
}

bool add_cluster_child(const std::uint32_t id, const std::uint8_t* d, const std::size_t n, Tracks& tracks) {
    std::uint64_t number = 0;
    if (id == kSimpleBlock) return add_block(d, n, tracks, number);
    if (id == kEncryptedBlock) {
        tracks.error = "encrypted block";
        return false;
    }
    if (id != kBlockGroup) return true;

    // what a player needs besides the frames: additions (alpha, hdr metadata) and discard padding
    const std::uint8_t* block = nullptr;
    std::size_t block_size = 0;
    Digest side;
    const bool parsed = for_children(d, n, [&](const std::uint32_t gid, const std::uint8_t* g, const std::size_t gn) {
        if (gid == kBlock) {
            if (block != nullptr) return false;
            block = g;
            block_size = gn;
        } else if (gid == kDiscardPadding) {
            side.add_number(gid);
            side.add_number(static_cast<std::uint64_t>(signed_value(g, gn)));
        } else if (gid == kBlockAdditions) {
            return for_children(g, gn, [&](const std::uint32_t mid, const std::uint8_t* m, const std::size_t mn) {
                if (mid != kBlockMore) return true;
                std::uint64_t add_id = 1;
                const std::uint8_t* extra = nullptr;
                std::size_t extra_size = 0;
                const bool more = for_children(m, mn, [&](const std::uint32_t aid, const std::uint8_t* a, const std::size_t an) {
                    if (aid == kBlockAddId) add_id = unsigned_value(a, an);
                    else if (aid == kBlockAdditional) extra = a, extra_size = an;
                    return true;
                });
                side.add_number(add_id);
                side.add_number(extra_size);
                if (extra != nullptr) side.add(extra, extra_size);
                return more;
            });
        }
        return true;
    });
    if (!parsed || block == nullptr || !add_block(block, block_size, tracks, number)) return false;
    if (side.bytes != 0) {
        Digest& to = tracks.by_number[number].side;
        to.add_number(tracks.by_number[number].frames);
        to.add_number(side.crc);
        to.add_number(side.adler);
        to.add_number(side.bytes);
    }
    return true;
}

/**
 * @brief Reads element headers and data straight from the file, for the levels too big to hold at once.
 */
class FileReader {
public:
    explicit FileReader(const fs::path& path) : in_(path, std::ios::binary) {
        std::error_code ec;
        size_ = fs::file_size(path, ec);
        if (ec) in_.setstate(std::ios::failbit);
    }
    [[nodiscard]] bool ok() const { return static_cast<bool>(in_); }
    [[nodiscard]] std::uint64_t size() const { return size_; }

    bool header(const std::uint64_t at, std::uint32_t& id, std::uint64_t& size, std::uint64_t& header_size) {
        std::array<std::uint8_t, 12> b{};
        const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(b.size(), size_ - std::min(size_, at)));
        if (n == 0 || !read(at, b.data(), n)) return false;
        std::size_t pos = 0;
        if (!parse_header(b.data(), n, pos, id, size)) return false;
        header_size = pos;
        return true;
    }

    bool read(const std::uint64_t at, std::uint8_t* out, const std::size_t n) {
        in_.clear();
        in_.seekg(static_cast<std::streamoff>(at));
        in_.read(reinterpret_cast<char*>(out), static_cast<std::streamsize>(n));
        return static_cast<std::size_t>(in_.gcount()) == n;
    }

private:
    std::ifstream in_;
    std::uint64_t size_ = 0;
};

bool parse_seek_head(const std::uint8_t* p, const std::size_t len, Tracks& tracks) {
    return for_children(p, len, [&](const std::uint32_t id, const std::uint8_t* d, const std::size_t n) {
        if (id != kSeek) return true;
        IndexEntry entry;
        bool have_position = false;
        const bool parsed = for_children(d, n, [&](const std::uint32_t sid, const std::uint8_t* v, const std::size_t vn) {
            if (sid == kSeekId) entry.id = static_cast<std::uint32_t>(unsigned_value(v, std::min<std::size_t>(vn, 4)));
            else if (sid == kSeekPosition) entry.position = unsigned_value(v, vn), have_position = true;
            return true;
        });
        if (!parsed || entry.id == 0 || !have_position) return false;
        tracks.index.push_back(entry);
        return true;
    });
}

bool parse_cues(const std::uint8_t* p, const std::size_t len, Tracks& tracks) {
    return for_children(p, len, [&](const std::uint32_t id, const std::uint8_t* d, const std::size_t n) {
        if (id != kCuePoint) return true;
        return for_children(d, n, [&](const std::uint32_t pid, const std::uint8_t* q, const std::size_t qn) {
            if (pid != kCueTrackPositions) return true;
            IndexEntry entry{.id = kCluster};
            bool have_cluster = false;
            const bool parsed = for_children(q, qn, [&](const std::uint32_t cid, const std::uint8_t* v, const std::size_t vn) {
                if (cid == kCueTrack) entry.track = unsigned_value(v, vn);
                else if (cid == kCueClusterPosition) entry.position = unsigned_value(v, vn), have_cluster = true;
                else if (cid == kCueRelativePosition) entry.block = unsigned_value(v, vn);
                return true;
            });
            if (!parsed || !have_cluster || entry.track == 0) return false;
            tracks.index.push_back(entry);
            return true;
        });
    });
}

std::string check_index(FileReader& file, const Tracks& tracks, const std::uint64_t segment_data, const std::uint64_t segment_end) {
    for (const IndexEntry& entry : tracks.index) {
        const char* what = entry.track != 0 ? "a cue" : "a SeekHead entry";
        if (entry.track != 0 && !tracks.by_number.contains(entry.track)) return std::string(what) + " names an undeclared track";
        std::uint32_t id = 0;
        std::uint64_t size = 0, head = 0;
        const std::uint64_t at = segment_data + entry.position;
        if (entry.position >= segment_end - segment_data || !file.header(at, id, size, head) || id != entry.id) {
            return std::string(what) + " points at the wrong place";
        }
        if (entry.block) {
            const std::uint64_t cluster_data = at + head;
            if (cluster_data >= segment_end || *entry.block >= segment_end - cluster_data ||
                !file.header(cluster_data + *entry.block, id, size, head) || (id != kSimpleBlock && id != kBlockGroup)) {
                return std::string(what) + " points at the wrong block";
            }
        }
    }
    return {};
}

/**
 * @brief Reads every track of a Matroska file; with verify_index, also requires the SeekHead and the cues
 * to point at what they claim and the segment to end within the file.
 */
Tracks read_tracks(const fs::path& path, const bool verify_index) {
    Tracks tracks;
    FileReader file(path);
    std::uint32_t id = 0;
    std::uint64_t size = 0, head = 0;
    if (!file.ok() || !file.header(0, id, size, head) || id != kEbmlHeader || size == kUnknownSize) {
        tracks.error = "not a Matroska file";
        return tracks;
    }
    std::uint64_t at = head + size;
    if (!file.header(at, id, size, head) || id != kSegment) {
        tracks.error = "no segment";
        return tracks;
    }
    at += head;
    const std::uint64_t segment_data = at;
    if (verify_index && size != kUnknownSize && size > file.size() - std::min(file.size(), at)) {
        tracks.error = "the segment runs past the end of the file";
        return tracks;
    }
    const std::uint64_t segment_end = size == kUnknownSize ? file.size() : std::min(file.size(), at + size);
    bool have_tracks = false;
    std::vector<std::uint8_t> buffer;

    while (at < segment_end) {
        if (!file.header(at, id, size, head)) break;
        const std::uint64_t data = at + head;
        if (id == kCluster && size == kUnknownSize) {
            // children up to the next top level element
            std::uint64_t child = data;
            while (child < segment_end) {
                std::uint32_t cid = 0;
                std::uint64_t csize = 0, chead = 0;
                if (!file.header(child, cid, csize, chead) || is_level1(cid)) break;
                if (chead > segment_end - child || csize == kUnknownSize || csize > segment_end - child - chead || csize > kMaxElement) {
                    tracks.error = "unreadable cluster";
                    return tracks;
                }
                buffer.resize(static_cast<std::size_t>(csize));
                if (!file.read(child + chead, buffer.data(), buffer.size()) ||
                    !add_cluster_child(cid, buffer.data(), buffer.size(), tracks)) {
                    if (tracks.error.empty()) tracks.error = "unreadable block";
                    return tracks;
                }
                child += chead + csize;
            }
            at = child;
            continue;
        }
        if (head > segment_end - at || size == kUnknownSize || size > segment_end - data) {
            tracks.error = "element running past the segment";
            return tracks;
        }
        if (id == kTracks || id == kCluster || (verify_index && (id == kSeekHead || id == kCues))) {
            if (size > kMaxElement) {
                tracks.error = "element too large to verify";
                return tracks;
            }
            buffer.resize(static_cast<std::size_t>(size));
            if (!file.read(data, buffer.data(), buffer.size())) {
                tracks.error = "truncated file";
                return tracks;
            }
            bool parsed = false;
            if (id == kTracks) {
                parsed = for_children(buffer.data(), buffer.size(), [&](const std::uint32_t tid, const std::uint8_t* t, const std::size_t tn) {
                    return tid != kTrackEntry || parse_track_entry(t, tn, tracks);
                });
            } else if (id == kCluster) {
                parsed = for_children(buffer.data(), buffer.size(), [&](const std::uint32_t cid, const std::uint8_t* c, const std::size_t cn) {
                    return add_cluster_child(cid, c, cn, tracks);
                });
            } else {
                parsed = id == kSeekHead ? parse_seek_head(buffer.data(), buffer.size(), tracks) : parse_cues(buffer.data(), buffer.size(), tracks);
            }
            if (!parsed) {
                if (tracks.error.empty()) {
                    tracks.error = id == kTracks ? "unreadable tracks" : id == kCluster ? "unreadable cluster" : id == kSeekHead ? "unreadable SeekHead" : "unreadable cues";
                }
                return tracks;
            }
            have_tracks = have_tracks || id == kTracks;
        }
        at = data + size;
    }
    if (!have_tracks) tracks.error = "no tracks";
    else if (verify_index) tracks.error = check_index(file, tracks, segment_data, segment_end);
    return tracks;
}

std::string name_of(const std::uint64_t number, const Track& t) {
    return "track " + std::to_string(number) + " (" + t.codec + ")";
}

std::string compare(const Tracks& ta, const Tracks& tb) {
    if (!tb.error.empty()) return "the result can't be read: " + tb.error;

    // tracks are matched by uid, then by number; a track without frames may be dropped
    std::map<std::uint64_t, std::uint64_t> number_of_uid;
    for (const auto& [number, u] : tb.by_number) {
        if (u.uid != 0) number_of_uid[u.uid] = number;
    }
    std::map<std::uint64_t, bool> matched;
    for (const auto& [number, t] : ta.by_number) {
        const auto by_uid = t.uid != 0 ? number_of_uid.find(t.uid) : number_of_uid.end();
        const std::uint64_t other = by_uid != number_of_uid.end() ? by_uid->second : number;
        const auto it = tb.by_number.find(other);
        if (it == tb.by_number.end() || matched[other]) {
            if (t.frames == 0) continue;
            return name_of(number, t) + " is missing";
        }
        matched[other] = true;
        const Track& u = it->second;
        if (t.uid != 0 && t.uid != u.uid) return name_of(number, t) + ": different track uid";
        if (t.codec != u.codec) return name_of(number, t) + " became " + u.codec;
        if (t.codec_private != u.codec_private) return name_of(number, t) + ": different codec private data";
        if (t.frames != u.frames || t.bytes != u.bytes) {
            return name_of(number, t) + ": " + std::to_string(t.frames) + " frames, " + std::to_string(t.bytes) +
                   " bytes, read back as " + std::to_string(u.frames) + " frames, " + std::to_string(u.bytes) + " bytes";
        }
        if (t.data != u.data) return name_of(number, t) + ": different frames";
        if (t.side != u.side) return name_of(number, t) + ": different block additions or discard padding";
    }
    for (const auto& [number, u] : tb.by_number) {
        if (!matched[number] && u.frames != 0) return name_of(number, u) + " appeared";
    }
    return {};
}

} // namespace

MkvTrackComparison compare_mkv_tracks(const fs::path& source, const fs::path& result) {
    const Tracks ta = read_tracks(source, false);
    if (!ta.error.empty()) return {.source_read = false, .difference = ta.error};
    return {.source_read = true, .difference = compare(ta, read_tracks(result, true))};
}

} // namespace chisel
