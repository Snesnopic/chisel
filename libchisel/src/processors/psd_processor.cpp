//
// Created by Giuseppe Francione on 24/09/26.
//

#include "../../include/psd_processor.hpp"
#include "../../include/file_type.hpp"
#include "../../include/file_utils.hpp"
#include "../../include/logger.hpp"
#include "../../include/random_utils.hpp"
#include <algorithm>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace chisel {

// named, so unity builds don't merge these with other files' helpers
namespace psd_format {
namespace {

using Bytes = std::vector<uint8_t>;
using View = std::span<const uint8_t>;

bool fits(const View d, const uint64_t at, const uint64_t size) {
    return at <= d.size() && size <= d.size() - at;
}

uint64_t read_be(const View d, const std::size_t at, const unsigned size) {
    uint64_t value = 0;
    for (unsigned i = 0; i < size; ++i) value = (value << 8) | d[at + i];
    return value;
}

void put_be(Bytes& out, const uint64_t value, const unsigned size) {
    for (unsigned i = size; i-- > 0;) out.push_back(static_cast<uint8_t>(value >> (8 * i)));
}

void set_be(Bytes& out, const std::size_t at, const uint64_t value, const unsigned size) {
    for (unsigned i = 0; i < size; ++i) out[at + i] = static_cast<uint8_t>(value >> (8 * (size - 1 - i)));
}

void append(Bytes& out, const View d, const std::size_t from, const std::size_t to) {
    out.insert(out.end(), d.begin() + static_cast<std::ptrdiff_t>(from), d.begin() + static_cast<std::ptrdiff_t>(to));
}

bool matches(const View d, const std::size_t at, const std::string_view s) {
    return fits(d, at, s.size()) && std::memcmp(d.data() + at, s.data(), s.size()) == 0;
}

// padding that gives a rebuilt length the alignment the original one had
std::size_t padding_for(const uint64_t original, const std::size_t length) {
    const std::size_t alignment = original % 4 == 0 ? 4 : original % 2 == 0 ? 2 : 1;
    return (alignment - length % alignment) % alignment;
}

struct Rect {
    int64_t top = 0, left = 0, bottom = 0, right = 0;
};

struct Channel {
    int16_t id = 0;
    std::size_t length_field = 0; ///< offset of the channel's length in its layer record
    std::size_t data = 0;         ///< offset of its compression field in the channel image data
    uint64_t length = 0;          ///< compression field included
};

struct Layer {
    Rect rect;
    std::optional<Rect> mask, real_mask;
    std::vector<Channel> channels;
};

/// @brief Layer count, records and channel image data, from the layer info or an Lr16/Lr32 block.
struct LayerInfo {
    std::size_t start = 0;       ///< the layer count
    std::size_t records_end = 0; ///< the first channel's data
    std::size_t data_end = 0;    ///< the end of the last channel's data
    std::size_t end = 0;         ///< padding included
    std::vector<Layer> layers;
};

/// @brief A file embedded in a linked layer block (smart object).
struct LinkedFile {
    std::size_t item = 0;       ///< the item's 8-byte length
    std::size_t item_end = 0;   ///< padding excluded
    std::size_t end = 0;        ///< padding included
    std::size_t size_field = 0; ///< the file's 8-byte size
    std::size_t file = 0;
    uint64_t file_size = 0;
};

/// @brief An additional layer information block.
struct Block {
    std::size_t start = 0; ///< the signature
    std::string key;
    unsigned length_size = 4;
    std::size_t data = 0;
    uint64_t length = 0;
    std::size_t end = 0;   ///< the next block
    std::optional<LayerInfo> layers;
    std::vector<LinkedFile> linked;
};

struct Resource {
    std::size_t start = 0;
    uint16_t id = 0;
    std::size_t size_field = 0;
    std::size_t data = 0;
    uint32_t size = 0;
    std::size_t end = 0;
};

struct Psd {
    bool big = false;
    uint64_t channels = 0, height = 0, width = 0, depth = 0;
    std::size_t resources_field = 0;
    std::size_t resources_end = 0;
    std::vector<Resource> resources;
    std::size_t layers_field = 0; ///< the layer and mask information section's length
    std::size_t layers_end = 0;
    std::size_t layer_info_field = 0;
    std::size_t layer_info_end = 0;
    std::optional<LayerInfo> layer_info;
    std::size_t global_mask = 0;
    std::vector<Block> blocks;
    std::size_t blocks_end = 0;
    std::size_t image_data = 0;
};

/// @brief An embedded file chisel can optimize: a JPEG thumbnail or a smart object's file.
struct Asset {
    std::size_t offset = 0;
    uint64_t size = 0;
    std::string extension;
};

unsigned length_size(const bool big) {
    return big ? 8 : 4;
}

std::optional<Rect> read_rect(const View d, const std::size_t at) {
    if (!fits(d, at, 16)) return std::nullopt;
    Rect r;
    r.top = static_cast<int32_t>(read_be(d, at, 4));
    r.left = static_cast<int32_t>(read_be(d, at + 4, 4));
    r.bottom = static_cast<int32_t>(read_be(d, at + 8, 4));
    r.right = static_cast<int32_t>(read_be(d, at + 12, 4));
    return r;
}

std::optional<LayerInfo> parse_layer_info(const View d, const std::size_t start, const std::size_t end, const bool big) {
    if (start + 2 > end || !fits(d, start, end - start)) return std::nullopt;
    LayerInfo info;
    info.start = start;
    info.end = end;
    const auto count = static_cast<int16_t>(read_be(d, start, 2));
    const std::size_t layers = count < 0 ? -static_cast<int32_t>(count) : count;
    const unsigned channel_length = length_size(big);
    std::size_t p = start + 2;
    for (std::size_t i = 0; i < layers; ++i) {
        Layer layer;
        const auto rect = read_rect(d, p);
        if (!rect || p + 18 > end) return std::nullopt;
        layer.rect = *rect;
        const auto channels = read_be(d, p + 16, 2);
        p += 18;
        if (channels > 56 || p + channels * (2 + channel_length) > end) return std::nullopt;
        for (uint64_t c = 0; c < channels; ++c) {
            Channel channel;
            channel.id = static_cast<int16_t>(read_be(d, p, 2));
            channel.length_field = p + 2;
            channel.length = read_be(d, p + 2, channel_length);
            layer.channels.push_back(channel);
            p += 2 + channel_length;
        }
        // blend mode signature and key, opacity, clipping, flags, filler, extra data length
        if (p + 16 > end || !matches(d, p, "8BIM")) return std::nullopt;
        const auto extra = read_be(d, p + 12, 4);
        const std::size_t extra_start = p + 16;
        if (extra > end - extra_start) return std::nullopt;
        // the mask rectangles give the row count of the mask channels
        if (extra >= 4) {
            const auto mask_size = read_be(d, extra_start, 4);
            if (mask_size >= 20 && mask_size <= extra - 4) {
                layer.mask = read_rect(d, extra_start + 4);
                if (mask_size >= 36) layer.real_mask = read_rect(d, extra_start + 4 + mask_size - 16);
            }
        }
        p = extra_start + extra;
        info.layers.push_back(std::move(layer));
    }
    info.records_end = p;
    for (auto& layer : info.layers) {
        for (auto& channel : layer.channels) {
            if (channel.length < 2 || channel.length > end - p) return std::nullopt;
            channel.data = p;
            p += channel.length;
        }
    }
    info.data_end = p;
    return info;
}

bool is_signature(const View d, const std::size_t at) {
    return matches(d, at, "8BIM") || matches(d, at, "8B64");
}

// keys whose length takes 8 bytes in a psb
bool has_big_length(const std::string_view key) {
    static constexpr std::array<std::string_view, 21> kBigKeys = {
        "Alph", "FELS", "FEid", "FMsk", "FXid", "LMsk", "Layr", "Lr16", "Lr32", "Mt16", "Mt32",
        "Mtrn", "PxSD", "artd", "cinf", "extd", "extn", "lnk2", "lnk3", "lnkE", "pths"
    };
    return std::ranges::find(kBigKeys, key) != kBigKeys.end();
}

std::string_view file_extension(const View d, const std::size_t at) {
    if (matches(d, at, "8BPS")) return fits(d, at, 6) && read_be(d, at + 4, 2) == 2 ? ".psb" : ".psd";
    if (matches(d, at, "\x89PNG")) return ".png";
    if (matches(d, at, "\xFF\xD8\xFF")) return ".jpg";
    if (matches(d, at, "%PDF")) return ".pdf";
    if (matches(d, at, "GIF8")) return ".gif";
    if (matches(d, at, std::string_view("II*\0", 4)) || matches(d, at, std::string_view("MM\0*", 4))) return ".tif";
    if (matches(d, at, "<?xml") || matches(d, at, "<svg")) return ".svg";
    return {};
}

// finds the embedded file of a linked data item; the fields around it only give its size
bool locate_linked_file(const View d, LinkedFile& f) {
    const std::size_t item = f.item + 8;
    if (!matches(d, item, "liFD") || f.item_end - item < 9) return false;
    const auto version = read_be(d, item + 4, 4);
    if (version < 1 || version > 8) return false;
    // unique id (pascal), file name (unicode), file type, creator
    std::size_t p = item + 8;
    p += 1 + d[p];
    if (!fits(d, p, 4)) return false;
    p += 4 + 2 * read_be(d, p, 4);
    p += 8;
    if (p + 9 > f.item_end) return false;
    f.size_field = p;
    f.file_size = read_be(d, p, 8);
    const bool descriptor = d[p + 8] != 0;
    p += 9;
    if (f.file_size > f.item_end - p) return false;

    // the child id, modification time and lock state follow the file in newer versions
    const auto ends_item = [&](const std::size_t file) {
        std::size_t q = file + f.file_size;
        if (version >= 5) {
            if (q + 4 > f.item_end) return false;
            q += 4 + 2 * read_be(d, q, 4);
        }
        if (version >= 6) q += 8;
        if (version >= 7) q += 1;
        return q == f.item_end;
    };
    if (!descriptor) {
        f.file = p;
        return ends_item(p) && !file_extension(d, p).empty();
    }
    // the open descriptor has no length: the file starts where the remaining fields end the item
    for (std::size_t file = p + 4; file + f.file_size <= f.item_end; ++file) {
        if (ends_item(file) && !file_extension(d, file).empty()) {
            f.file = file;
            return true;
        }
    }
    return false;
}

void parse_linked(const View d, Block& block) {
    const std::size_t end = block.data + block.length;
    for (std::size_t p = block.data; p + 8 <= end;) {
        LinkedFile f;
        f.item = p;
        const auto length = read_be(d, p, 8);
        if (length > end - (p + 8)) return;
        f.item_end = p + 8 + length;
        f.end = std::min<std::size_t>(end, f.item_end + (4 - length % 4) % 4);
        if (locate_linked_file(d, f)) block.linked.push_back(f);
        p = f.end;
    }
}

void parse_blocks(const View d, std::size_t p, const std::size_t end, const bool big, Psd& psd) {
    psd.blocks_end = p;
    while (p + 12 <= end && is_signature(d, p)) {
        Block block;
        block.start = p;
        block.key.assign(reinterpret_cast<const char*>(d.data() + p + 4), 4);
        const unsigned preferred = big && has_big_length(block.key) ? 8 : 4;
        bool parsed = false;
        // psb writers disagree on some keys: accept the other length size when it's the one that fits
        for (const unsigned size : {preferred, big ? 12u - preferred : preferred}) {
            if (p + 8 + size > end) continue;
            const auto length = read_be(d, p + 8, size);
            const std::size_t data = p + 8 + size;
            if (length > end - data) continue;
            std::size_t next = data + length;
            while (next < end && next - (data + length) < 4 && !is_signature(d, next)) ++next;
            if (next != end && !is_signature(d, next)) continue;
            block.length_size = size;
            block.data = data;
            block.length = length;
            block.end = next;
            parsed = true;
            break;
        }
        if (!parsed) return;
        if (block.key == "Lr16" || block.key == "Lr32" || block.key == "Layr") {
            block.layers = parse_layer_info(d, block.data, block.data + block.length, big);
        } else if (block.key == "lnk2" || block.key == "lnk3" || block.key == "lnkD") {
            parse_linked(d, block);
        }
        p = block.end;
        psd.blocks.push_back(std::move(block));
        psd.blocks_end = p;
    }
}

std::optional<Psd> parse(const View d) {
    if (!matches(d, 0, "8BPS") || !fits(d, 0, 26)) return std::nullopt;
    const auto version = read_be(d, 4, 2);
    if (version != 1 && version != 2) return std::nullopt;
    Psd psd;
    psd.big = version == 2;
    psd.channels = read_be(d, 12, 2);
    psd.height = read_be(d, 14, 4);
    psd.width = read_be(d, 18, 4);
    psd.depth = read_be(d, 22, 2);
    std::size_t p = 26;

    // color mode data
    if (!fits(d, p, 4) || !fits(d, p + 4, read_be(d, p, 4))) return std::nullopt;
    p += 4 + read_be(d, p, 4);

    // image resources
    psd.resources_field = p;
    if (!fits(d, p, 4) || !fits(d, p + 4, read_be(d, p, 4))) return std::nullopt;
    psd.resources_end = p + 4 + read_be(d, p, 4);
    for (std::size_t r = p + 4; r + 12 <= psd.resources_end && matches(d, r, "8BIM");) {
        Resource resource;
        resource.start = r;
        resource.id = static_cast<uint16_t>(read_be(d, r + 4, 2));
        // pascal name padded to an even length
        const std::size_t size_field = r + 6 + ((d[r + 6] + 2) & ~std::size_t{1});
        if (size_field + 4 > psd.resources_end) break;
        resource.size_field = size_field;
        resource.size = static_cast<uint32_t>(read_be(d, size_field, 4));
        resource.data = size_field + 4;
        if (resource.size > psd.resources_end - resource.data) break;
        resource.end = std::min<std::size_t>(psd.resources_end, resource.data + resource.size + (resource.size & 1));
        psd.resources.push_back(resource);
        r = resource.end;
    }

    // layer and mask information
    const unsigned size = length_size(psd.big);
    p = psd.resources_end;
    psd.layers_field = p;
    if (!fits(d, p, size) || !fits(d, p + size, read_be(d, p, size))) return std::nullopt;
    psd.layers_end = p + size + read_be(d, p, size);
    p += size;
    psd.layer_info_field = p;
    psd.layer_info_end = p;
    psd.global_mask = p;
    psd.blocks_end = p;
    if (p < psd.layers_end) {
        if (p + size > psd.layers_end) return std::nullopt;
        const auto length = read_be(d, p, size);
        if (length > psd.layers_end - (p + size)) return std::nullopt;
        psd.layer_info_end = p + size + length;
        if (length > 0) psd.layer_info = parse_layer_info(d, p + size, psd.layer_info_end, psd.big);
        p = psd.layer_info_end;
        psd.global_mask = p;
        if (p + 4 <= psd.layers_end) {
            const auto mask = read_be(d, p, 4);
            if (mask > psd.layers_end - (p + 4)) return std::nullopt;
            p += 4 + mask;
        }
        parse_blocks(d, p, psd.layers_end, psd.big, psd);
    }

    psd.image_data = psd.layers_end;
    if (!fits(d, psd.image_data, 2)) return std::nullopt;
    return psd;
}

// decodes one packbits row, which must give exactly out.size() bytes
bool unpack_row(const View in, Bytes& out) {
    std::size_t p = 0, o = 0;
    while (p < in.size()) {
        const auto header = static_cast<int8_t>(in[p++]);
        if (header >= 0) {
            const std::size_t n = static_cast<std::size_t>(header) + 1;
            if (n > in.size() - p || n > out.size() - o) return false;
            std::memcpy(out.data() + o, in.data() + p, n);
            p += n;
            o += n;
        } else if (header != -128) {
            const auto n = static_cast<std::size_t>(1 - static_cast<int>(header));
            if (p >= in.size() || n > out.size() - o) return false;
            std::memset(out.data() + o, in[p++], n);
            o += n;
        }
    }
    return o == out.size();
}

// shortest packbits encoding of a row: literal runs cost 1 + n bytes, repeat runs 2 bytes
void pack_row(const View row, Bytes& out) {
    const std::size_t n = row.size();
    constexpr std::size_t kMaxRun = 128;
    // equal bytes starting at each position
    std::vector<uint32_t> run(n);
    for (std::size_t i = n; i-- > 0;) run[i] = i + 1 < n && row[i] == row[i + 1] ? run[i + 1] + 1 : 1;

    std::vector<uint64_t> cost(n + 1, UINT64_MAX);
    std::vector<uint32_t> from(n + 1, 0);
    std::vector<uint8_t> repeat(n + 1, 0);
    cost[0] = 0;
    // literal starts with increasing cost[j] - j, so the front is the cheapest literal ending here
    std::deque<std::size_t> starts;
    const auto key = [&](const std::size_t j) { return static_cast<int64_t>(cost[j]) - static_cast<int64_t>(j); };
    for (std::size_t t = 0; t <= n; ++t) {
        while (!starts.empty() && starts.front() + kMaxRun < t) starts.pop_front();
        if (!starts.empty()) {
            const auto j = starts.front();
            if (const auto c = cost[j] + 1 + (t - j); c < cost[t]) {
                cost[t] = c;
                from[t] = static_cast<uint32_t>(j);
                repeat[t] = 0;
            }
        }
        if (t == n) break;
        while (!starts.empty() && key(starts.back()) >= key(t)) starts.pop_back();
        starts.push_back(t);
        if (run[t] >= 2) {
            const auto to = t + std::min<std::size_t>(run[t], kMaxRun);
            if (cost[t] + 2 < cost[to]) {
                cost[to] = cost[t] + 2;
                from[to] = static_cast<uint32_t>(t);
                repeat[to] = 1;
            }
        }
    }

    std::vector<std::size_t> cuts;
    for (std::size_t t = n; t > 0; t = from[t]) cuts.push_back(t);
    std::size_t start = 0;
    for (auto it = cuts.rbegin(); it != cuts.rend(); ++it) {
        const std::size_t length = *it - start;
        if (repeat[*it]) {
            out.push_back(static_cast<uint8_t>(257 - length));
            out.push_back(row[start]);
        } else {
            out.push_back(static_cast<uint8_t>(length - 1));
            out.insert(out.end(), row.begin() + static_cast<std::ptrdiff_t>(start), row.begin() + static_cast<std::ptrdiff_t>(*it));
        }
        start = *it;
    }
}

/**
 * @brief Re-encodes rle data (row byte counts, then packbits rows) with the shortest encoding.
 * @param consumed Set to the bytes the rle data took, which may be less than length.
 * @return The new counts and rows, or std::nullopt if the data isn't well-formed rle.
 */
std::optional<Bytes> repack_rows(const View d, const std::size_t at, const uint64_t length, const uint64_t rows,
                                 const uint64_t row_bytes, const unsigned count_size, uint64_t& consumed) {
    if (rows == 0 || row_bytes == 0 || rows > length / count_size || row_bytes > (1u << 30)) return std::nullopt;
    const uint64_t table = rows * count_size;
    uint64_t total = table;
    for (uint64_t r = 0; r < rows; ++r) total += read_be(d, at + r * count_size, count_size);
    if (total > length) return std::nullopt;

    Bytes counts, packed, row(row_bytes);
    const uint64_t max_count = count_size == 2 ? 0xFFFF : 0xFFFFFFFF;
    std::size_t p = at + table;
    for (uint64_t r = 0; r < rows; ++r) {
        const auto count = read_be(d, at + r * count_size, count_size);
        if (!unpack_row(d.subspan(p, count), row)) return std::nullopt;
        const auto before = packed.size();
        pack_row(row, packed);
        if (packed.size() - before > max_count) return std::nullopt;
        put_be(counts, packed.size() - before, count_size);
        p += count;
    }
    consumed = total;
    counts.insert(counts.end(), packed.begin(), packed.end());
    return counts;
}

std::optional<Bytes> repack_channel(const View d, const Psd& psd, const Layer& layer, const Channel& channel) {
    std::vector<Rect> rects;
    if (channel.id == -2 && layer.mask) rects.push_back(*layer.mask);
    if (channel.id == -3 && layer.real_mask) rects.push_back(*layer.real_mask);
    rects.push_back(layer.rect);
    // the row counts must add up to the channel's length for the rectangle to be the right one
    for (const auto& r : rects) {
        const auto rows = r.bottom - r.top;
        const auto width = r.right - r.left;
        if (rows <= 0 || width <= 0) continue;
        uint64_t consumed = 0;
        auto packed = repack_rows(d, channel.data + 2, channel.length - 2, static_cast<uint64_t>(rows),
                                  (static_cast<uint64_t>(width) * psd.depth + 7) / 8, psd.big ? 4 : 2, consumed);
        if (packed && consumed == channel.length - 2) return packed;
    }
    return std::nullopt;
}

Bytes rebuild_layer_info(const View d, const Psd& psd, const LayerInfo& info, const bool repack) {
    Bytes out;
    append(out, d, info.start, info.records_end);
    const unsigned size = length_size(psd.big);
    for (const auto& layer : info.layers) {
        for (const auto& channel : layer.channels) {
            const auto before = out.size();
            std::optional<Bytes> packed;
            if (repack && read_be(d, channel.data, 2) == 1) packed = repack_channel(d, psd, layer, channel);
            if (packed && packed->size() + 2 < channel.length) {
                put_be(out, 1, 2);
                out.insert(out.end(), packed->begin(), packed->end());
            } else {
                append(out, d, channel.data, channel.data + channel.length);
            }
            set_be(out, channel.length_field - info.start, out.size() - before, size);
        }
    }
    if (out.size() == info.data_end - info.start) {
        append(out, d, info.data_end, info.end);
    } else {
        out.resize(out.size() + padding_for(info.end - info.start, out.size()), 0);
    }
    return out;
}

Bytes rebuild_linked(const View d, const Block& block, const std::map<std::size_t, Bytes>& payloads) {
    Bytes out;
    std::size_t copied = block.data;
    for (const auto& f : block.linked) {
        const auto it = payloads.find(f.file);
        if (it == payloads.end()) continue;
        append(out, d, copied, f.item);
        Bytes item;
        append(item, d, f.item + 8, f.size_field);
        put_be(item, it->second.size(), 8);
        append(item, d, f.size_field + 8, f.file);
        item.insert(item.end(), it->second.begin(), it->second.end());
        append(item, d, f.file + f.file_size, f.item_end);
        put_be(out, item.size(), 8);
        out.insert(out.end(), item.begin(), item.end());
        out.resize(out.size() + (4 - item.size() % 4) % 4, 0);
        copied = f.end;
    }
    append(out, d, copied, block.data + block.length);
    return out;
}

bool is_thumbnail(const View d, const Resource& resource) {
    // photoshop 5+ and 4 thumbnails, format 1 being jfif
    return (resource.id == 1036 || resource.id == 1033) && resource.size > 28 && read_be(d, resource.data, 4) == 1 &&
           matches(d, resource.data + 28, "\xFF\xD8\xFF");
}

std::vector<Asset> find_assets(const View d, const Psd& psd) {
    std::vector<Asset> assets;
    for (const auto& resource : psd.resources) {
        if (is_thumbnail(d, resource)) assets.push_back({resource.data + 28, resource.size - 28u, ".jpg"});
    }
    for (const auto& block : psd.blocks) {
        for (const auto& f : block.linked) {
            assets.push_back({f.file, f.file_size, std::string(file_extension(d, f.file))});
        }
    }
    return assets;
}

struct Edits {
    bool repack = false;
    std::map<std::size_t, Bytes> payloads; ///< new content of the embedded file starting at this offset
};

Bytes rebuild(const View d, const Psd& psd, const Edits& edits) {
    Bytes out;
    append(out, d, 0, psd.resources_field);

    // image resources
    Bytes resources;
    std::size_t copied = psd.resources_field + 4;
    for (const auto& resource : psd.resources) {
        const auto it = is_thumbnail(d, resource) ? edits.payloads.find(resource.data + 28) : edits.payloads.end();
        if (it == edits.payloads.end()) continue;
        append(resources, d, copied, resource.size_field);
        put_be(resources, 28 + it->second.size(), 4);
        Bytes header;
        append(header, d, resource.data, resource.data + 28);
        set_be(header, 20, it->second.size(), 4);
        resources.insert(resources.end(), header.begin(), header.end());
        resources.insert(resources.end(), it->second.begin(), it->second.end());
        if (it->second.size() % 2 != 0) resources.push_back(0);
        copied = resource.end;
    }
    append(resources, d, copied, psd.resources_end);
    put_be(out, resources.size(), 4);
    out.insert(out.end(), resources.begin(), resources.end());

    // layer and mask information
    const unsigned size = length_size(psd.big);
    Bytes layers;
    if (psd.layer_info) {
        const auto info = rebuild_layer_info(d, psd, *psd.layer_info, edits.repack);
        put_be(layers, info.size(), size);
        layers.insert(layers.end(), info.begin(), info.end());
    } else {
        append(layers, d, psd.layer_info_field, psd.layer_info_end);
    }
    append(layers, d, psd.layer_info_end, psd.blocks.empty() ? psd.blocks_end : psd.blocks.front().start);
    for (const auto& block : psd.blocks) {
        std::optional<Bytes> data;
        if (block.layers && edits.repack) {
            data = rebuild_layer_info(d, psd, *block.layers, true);
        } else if (!block.linked.empty() && !edits.payloads.empty()) {
            data = rebuild_linked(d, block, edits.payloads);
        }
        if (!data || (data->size() == block.length && std::equal(data->begin(), data->end(), d.begin() + static_cast<std::ptrdiff_t>(block.data)))) {
            append(layers, d, block.start, block.end);
            continue;
        }
        append(layers, d, block.start, block.start + 8);
        put_be(layers, data->size(), block.length_size);
        layers.insert(layers.end(), data->begin(), data->end());
        layers.resize(layers.size() + padding_for(block.end - block.data, data->size()), 0);
    }
    append(layers, d, psd.blocks_end, psd.layers_end);
    if (psd.layers_end > psd.layers_field + size) {
        put_be(out, layers.size(), size);
        out.insert(out.end(), layers.begin(), layers.end());
    } else {
        append(out, d, psd.layers_field, psd.layers_end);
    }

    // composite image
    uint64_t consumed = 0;
    std::optional<Bytes> packed;
    if (edits.repack && read_be(d, psd.image_data, 2) == 1) {
        packed = repack_rows(d, psd.image_data + 2, d.size() - psd.image_data - 2, psd.channels * psd.height,
                             (psd.width * psd.depth + 7) / 8, psd.big ? 4 : 2, consumed);
    }
    if (packed && packed->size() < consumed) {
        put_be(out, 1, 2);
        out.insert(out.end(), packed->begin(), packed->end());
        append(out, d, psd.image_data + 2 + consumed, d.size());
    } else {
        append(out, d, psd.image_data, d.size());
    }
    return out;
}

std::optional<Bytes> read_bytes(const std::filesystem::path& path) {
    Bytes data;
    if (!read_file(path, data)) return std::nullopt;
    return data;
}

bool write_bytes(const std::filesystem::path& path, const Bytes& data) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    out.close();
    return static_cast<bool>(out);
}

/// @brief What finalize needs to find the extracted files again.
struct ExtractedAsset {
    uint64_t size = 0;
    std::size_t hash = 0;
};

std::size_t hash_of(const View d, const std::size_t at, const uint64_t size) {
    return std::hash<std::string_view>{}(std::string_view(reinterpret_cast<const char*>(d.data() + at), size));
}

} // namespace
} // namespace psd_format

void PsdProcessor::recompress(const std::filesystem::path& input_path, const std::filesystem::path& output_path,
                              const ProcessingOptions& options) {
    using namespace psd_format;
    Logger::log(LogLevel::Debug, "Entering recompress for " + input_path.filename().string(), get_name());

    const auto data = read_bytes(input_path);
    if (!data) throw std::runtime_error("can't read " + input_path.string());
    const auto psd = parse(*data);
    if (!psd) Logger::log(LogLevel::Debug, "Unsupported document layout, left as is", get_name());
    if (!write_bytes(output_path, psd ? rebuild(*data, *psd, {.repack = true}) : *data)) {
        throw std::runtime_error("can't write " + output_path.string());
    }

    Logger::log(LogLevel::Debug, "Exiting recompress for " + output_path.filename().string(), get_name());
}

std::optional<ExtractedContent> PsdProcessor::prepare_extraction(const std::filesystem::path& input_path) {
    using namespace psd_format;
    Logger::log(LogLevel::Debug, "Entering prepare_extraction for " + input_path.filename().string(), get_name());

    ExtractedContent content;
    content.original_path = input_path;
    content.format = ContainerFormat::Unknown;
    std::vector<ExtractedAsset> extracted;
    const auto data = read_bytes(input_path);
    const auto psd = data ? parse(*data) : std::nullopt;
    for (const auto& asset : psd ? find_assets(*data, *psd) : std::vector<Asset>{}) {
        if (content.temp_dir.empty()) content.temp_dir = make_temp_dir_for(input_path, "psd");
        const auto file = content.temp_dir / ("embedded_" + std::to_string(extracted.size()) + asset.extension);
        const Bytes bytes(data->begin() + static_cast<std::ptrdiff_t>(asset.offset),
                          data->begin() + static_cast<std::ptrdiff_t>(asset.offset + asset.size));
        if (!write_bytes(file, bytes)) {
            Logger::log(LogLevel::Warning, "Can't write " + file.string(), get_name());
            continue;
        }
        content.extracted_files.push_back(file);
        extracted.push_back({asset.size, hash_of(*data, asset.offset, asset.size)});
    }
    Logger::log(LogLevel::Debug, "Extracted " + std::to_string(extracted.size()) + " embedded files", get_name());
    content.extras = std::move(extracted);

    Logger::log(LogLevel::Debug, "Exiting prepare_extraction for " + input_path.filename().string(), get_name());
    return content;
}

std::filesystem::path PsdProcessor::finalize_extraction(const ExtractedContent& content, const ProcessingOptions& options) {
    using namespace psd_format;
    Logger::log(LogLevel::Debug, "Entering finalize_extraction for " + content.original_path.filename().string(), get_name());

    const auto cleanup = [&] {
        if (!content.temp_dir.empty()) cleanup_temp_dir(content.temp_dir, get_name());
    };
    const auto* extracted = std::any_cast<std::vector<ExtractedAsset>>(&content.extras);
    if (extracted == nullptr || extracted->empty() || extracted->size() != content.extracted_files.size()) {
        cleanup();
        return {};
    }

    // phase 2 may have repacked the document since, which leaves the embedded files as they were
    const auto data = read_bytes(content.original_path);
    const auto psd = data ? parse(*data) : std::nullopt;
    const auto assets = psd ? find_assets(*data, *psd) : std::vector<Asset>{};
    if (assets.size() != extracted->size()) {
        Logger::log(LogLevel::Warning, "Embedded files changed since extraction in " + content.original_path.filename().string(), get_name());
        cleanup();
        return {};
    }
    Edits edits;
    for (std::size_t i = 0; i < assets.size(); ++i) {
        if (assets[i].size != (*extracted)[i].size || hash_of(*data, assets[i].offset, assets[i].size) != (*extracted)[i].hash) {
            Logger::log(LogLevel::Warning, "Embedded files changed since extraction in " + content.original_path.filename().string(), get_name());
            cleanup();
            return {};
        }
        Bytes optimized;
        if (read_file(content.extracted_files[i], optimized) && !optimized.empty() && optimized.size() < assets[i].size) {
            edits.payloads.emplace(assets[i].offset, std::move(optimized));
        }
    }
    cleanup();
    if (edits.payloads.empty()) {
        Logger::log(LogLevel::Debug, "No embedded file got smaller", get_name());
        return {};
    }

    const auto out_path = std::filesystem::temp_directory_path() /
                          (content.original_path.stem().string() + "_tmp" + RandomUtils::random_suffix() +
                           content.original_path.extension().string());
    if (!write_bytes(out_path, rebuild(*data, *psd, edits))) {
        Logger::log(LogLevel::Error, "Can't write " + out_path.string(), get_name());
        std::error_code ec;
        std::filesystem::remove(out_path, ec);
        return {};
    }

    Logger::log(LogLevel::Debug, "Exiting finalize_extraction for " + out_path.filename().string(), get_name());
    return out_path;
}

std::string PsdProcessor::get_raw_checksum(const std::filesystem::path& /*file_path*/) const {
    return "";
}

bool PsdProcessor::raw_equal(const std::filesystem::path& a, const std::filesystem::path& b) const {
    using namespace psd_format;
    // repacking is deterministic, so equal decoded channels give equal canonical documents
    const auto data_a = read_bytes(a);
    const auto data_b = read_bytes(b);
    if (data_a && data_b && *data_a == *data_b) return true;
    const auto psd_a = data_a ? parse(*data_a) : std::nullopt;
    const auto psd_b = data_b ? parse(*data_b) : std::nullopt;
    return psd_a && psd_b && rebuild(*data_a, *psd_a, {.repack = true}) == rebuild(*data_b, *psd_b, {.repack = true});
}

} // namespace chisel
