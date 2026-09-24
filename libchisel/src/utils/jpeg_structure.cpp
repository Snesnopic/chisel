//
// Created by Giuseppe Francione on 23/09/26.
//

#include "../../include/jpeg_structure.hpp"
#include <pugixml.hpp>
#include <algorithm>
#include <cstring>
#include <map>
#include <set>

namespace chisel::jpeg {

namespace {

constexpr std::string_view kRdfNs = "http://www.w3.org/1999/02/22-rdf-syntax-ns#";
constexpr std::string_view kHdrgmNs = "http://ns.adobe.com/hdr-gain-map/1.0/";
constexpr std::string_view kContainerNs = "http://ns.google.com/photos/1.0/container/";
constexpr std::string_view kItemNs = "http://ns.google.com/photos/1.0/container/item/";
constexpr std::string_view kGCameraNs = "http://ns.google.com/photos/1.0/camera/";
constexpr std::string_view kAppleGainMapNs = "http://ns.apple.com/HDRGainMap/1.0/";
constexpr std::string_view kApdiNs = "http://ns.apple.com/pixeldatainfo/1.0/";

using Namespaces = std::map<std::string, std::string, std::less<>>;

bool matches(const std::span<const uint8_t> d, const std::size_t at, const std::string_view s) {
    return at + s.size() <= d.size() && std::memcmp(&d[at], s.data(), s.size()) == 0;
}

uint16_t read16(const std::span<const uint8_t> d, const std::size_t at, const bool le) {
    const unsigned b0 = d[at], b1 = d[at + 1];
    return static_cast<uint16_t>(le ? (b0 | b1 << 8) : (b0 << 8 | b1));
}

/**
 * @brief Validates the AFCP trailer whose 12-byte footer ends at @p end.
 * @param base Absolute file offset of d[0], since AFCP offsets are absolute.
 * @return Start of the trailer within @p d, or std::nullopt.
 */
std::optional<std::size_t> afcp_start(const std::span<const uint8_t> d, const std::size_t from,
                                      const std::size_t end, const std::size_t base) {
    const bool le = d[end - 9] == '*';
    const std::size_t abs = read32(d, end - 8, le);
    if (abs < base) return std::nullopt;
    const std::size_t start = abs - base;
    if (start < from || start + 12 > end - 12 || std::memcmp(&d[start], &d[end - 12], 4) != 0) return std::nullopt;
    const std::size_t dir = start + 12;
    const std::size_t n = read16(d, start + 6, le);
    if (dir + 12 * n > end - 12) return std::nullopt;
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t size = read32(d, dir + 12 * i + 4, le);
        const std::size_t off = read32(d, dir + 12 * i + 8, le);
        if (off < base + start || off - base + size > end - 12) return std::nullopt;
    }
    return start;
}

/**
 * @brief Walks the IPTC trailers stacked at the end of @p d, newest first.
 * @param on_afcp Called with the start and end of each AFCP trailer.
 * @return Start of the whole stack.
 */
template <typename F>
std::size_t walk_trailers(const std::span<const uint8_t> d, const std::size_t from, const std::size_t base, F&& on_afcp) {
    std::size_t end = d.size();
    while (end > from) {
        const std::size_t avail = end - from;
        if (avail >= 24 && (matches(d, end - 12, "AXS!") || matches(d, end - 12, "AXS*"))) {
            const auto start = afcp_start(d, from, end, base);
            if (!start) break;
            on_afcp(*start, end);
            end = *start;
        } else if (avail >= 10 && matches(d, end - 4, "\xA1\xB2\xC3\xD4")) {
            // fotostation: chain of records, each ending in tag(2) size(4) signature(4)
            std::size_t p = end;
            while (p - from >= 10 && matches(d, p - 4, "\xA1\xB2\xC3\xD4")) {
                const std::size_t size = read32(d, p - 8, false);
                if (size < 10 || size > p - from) break;
                p -= size;
            }
            if (p == end) break;
            end = p;
        } else if (avail >= 12 && matches(d, end - 8, "cbipcbbl")) {
            const std::size_t size = read32(d, end - 12, false);
            if (size > avail - 12) break;
            end -= size + 12;
        } else {
            break;
        }
    }
    return end;
}

template <typename F>
void for_each_element(const pugi::xml_node node, F&& f) {
    for (pugi::xml_node c = node.first_child(); c; c = c.next_sibling()) {
        if (c.type() != pugi::node_element) continue;
        f(c);
        for_each_element(c, f);
    }
}

Namespaces namespaces(const pugi::xml_document& doc) {
    Namespaces ns;
    for_each_element(doc, [&](const pugi::xml_node e) {
        for (const pugi::xml_attribute a : e.attributes()) {
            const std::string_view name = a.name();
            if (name.starts_with("xmlns:")) ns[std::string(name.substr(6))] = a.value();
        }
    });
    return ns;
}

std::string_view prefix_of(const std::string_view qname) {
    const auto colon = qname.find(':');
    return colon == std::string_view::npos ? std::string_view{} : qname.substr(0, colon);
}

std::string prefix_for(const Namespaces& ns, const std::string_view uri) {
    for (const auto& [prefix, value] : ns) {
        if (value == uri) return prefix;
    }
    return {};
}

bool is_link_property(const std::string_view qname, const Namespaces& ns) {
    const auto it = ns.find(prefix_of(qname));
    if (it == ns.end()) return false;
    const std::string_view uri = it->second;
    if (uri == kHdrgmNs || uri == kContainerNs || uri == kItemNs || uri == kAppleGainMapNs || uri == kApdiNs) {
        return true;
    }
    const std::string_view local = qname.substr(qname.find(':') + 1);
    return uri == kGCameraNs && (local.starts_with("MotionPhoto") || local.starts_with("MicroVideo"));
}

void collect_prefixes(const pugi::xml_node node, std::set<std::string, std::less<>>& out) {
    out.emplace(prefix_of(node.name()));
    for (const pugi::xml_attribute a : node.attributes()) {
        out.emplace(prefix_of(a.name()));
    }
    for_each_element(node, [&](const pugi::xml_node e) {
        out.emplace(prefix_of(e.name()));
        for (const pugi::xml_attribute a : e.attributes()) out.emplace(prefix_of(a.name()));
    });
}

std::optional<std::span<const uint8_t>> app1_payload(const std::span<const uint8_t> image, const std::size_t limit,
                                                     const std::string_view id) {
    std::size_t p = 2;
    while (p + 4 <= limit && image[p] == 0xFF) {
        const uint8_t marker = image[p + 1];
        if (marker == 0xDA || marker == 0xD9) break;
        const std::size_t len = (static_cast<std::size_t>(image[p + 2]) << 8) | image[p + 3];
        if (len < 2 || p + 2 + len > limit) break;
        if (marker == 0xE1 && len - 2 >= id.size() && matches(image, p + 4, id)) {
            return image.subspan(p + 4, len - 2);
        }
        p += 2 + len;
    }
    return std::nullopt;
}

std::optional<bool> byte_order(const std::span<const uint8_t> d, const std::size_t at) {
    if (matches(d, at, "II")) return true;
    if (matches(d, at, "MM")) return false;
    return std::nullopt;
}

/// @brief Offset of the 12-byte entry for @p tag in the IFD at @p ifd, if present.
std::optional<std::size_t> ifd_entry(const std::span<const uint8_t> d, const std::size_t ifd, const uint16_t tag,
                                     const bool le) {
    if (ifd > d.size() || d.size() - ifd < 2) return std::nullopt;
    const std::size_t n = read16(d, ifd, le);
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t e = ifd + 2 + 12 * i;
        if (e + 12 > d.size()) return std::nullopt;
        if (read16(d, e, le) == tag) return e;
    }
    return std::nullopt;
}

std::size_t tiff_type_size(const uint16_t type) {
    switch (type) {
        case 1: case 2: case 6: case 7: return 1;
        case 3: case 8: return 2;
        case 4: case 9: case 11: return 4;
        case 5: case 10: case 12: return 8;
        default: return 0;
    }
}

void put16(std::vector<uint8_t>& out, const uint16_t v, const bool le) {
    out.push_back(static_cast<uint8_t>(le ? v : v >> 8));
    out.push_back(static_cast<uint8_t>(le ? v >> 8 : v));
}

void put32(std::vector<uint8_t>& out, const uint32_t v, const bool le) {
    for (std::size_t i = 0; i < 4; ++i) {
        out.push_back(static_cast<uint8_t>(v >> (8 * (le ? i : 3 - i))));
    }
}

std::string serialize(const pugi::xml_document& doc) {
    struct Writer final : pugi::xml_writer {
        std::string out;
        void write(const void* data, const std::size_t size) override {
            out.append(static_cast<const char*>(data), size);
        }
    } writer;
    doc.save(writer, "", pugi::format_raw | pugi::format_no_declaration, pugi::encoding_utf8);
    return writer.out;
}

} // namespace

uint32_t read32(const std::span<const uint8_t> d, const std::size_t at, const bool little_endian) {
    const uint32_t b0 = d[at], b1 = d[at + 1], b2 = d[at + 2], b3 = d[at + 3];
    return little_endian ? (b0 | b1 << 8 | b2 << 16 | b3 << 24) : (b0 << 24 | b1 << 16 | b2 << 8 | b3);
}

void write32(const std::span<uint8_t> d, const std::size_t at, const uint32_t value, const bool little_endian) {
    for (std::size_t i = 0; i < 4; ++i) {
        const std::size_t shift = 8 * (little_endian ? i : 3 - i);
        d[at + i] = static_cast<uint8_t>(value >> shift);
    }
}

std::optional<std::size_t> first_image_end(const std::span<const uint8_t> data) {
    const std::size_t n = data.size();
    if (n < 4 || data[0] != 0xFF || data[1] != 0xD8) return std::nullopt;
    std::size_t p = 2;
    while (true) {
        if (p >= n || data[p] != 0xFF) return std::nullopt;
        while (p < n && data[p] == 0xFF) ++p;
        if (p >= n) return std::nullopt;
        const uint8_t marker = data[p++];
        if (marker == 0xD9) return p;
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue;
        if (p + 2 > n) return std::nullopt;
        const std::size_t len = (static_cast<std::size_t>(data[p]) << 8) | data[p + 1];
        if (len < 2 || p + len > n) return std::nullopt;
        p += len;
        if (marker == 0xDA) {
            // entropy-coded data: only stuffed zeros, restart markers and fill bytes may follow 0xFF
            while (p + 1 < n) {
                if (data[p] == 0xFF) {
                    const uint8_t next = data[p + 1];
                    if (next == 0x00 || (next >= 0xD0 && next <= 0xD7)) { p += 2; continue; }
                    if (next == 0xFF) { ++p; continue; }
                    break;
                }
                ++p;
            }
            if (p + 1 >= n) return std::nullopt;
        }
    }
}

std::optional<MpfIndex> find_mpf_index(const std::span<const uint8_t> image, const std::size_t limit) {
    const auto& d = image;
    std::size_t p = 2;
    while (p + 4 <= limit && d[p] == 0xFF) {
        const uint8_t marker = d[p + 1];
        if (marker == 0xDA || marker == 0xD9) return std::nullopt;
        const std::size_t len = (static_cast<std::size_t>(d[p + 2]) << 8) | d[p + 3];
        const std::size_t seg = p + 4;
        const std::size_t seg_end = p + 2 + len;
        if (len < 2 || seg_end > limit) return std::nullopt;
        if (marker == 0xE2 && seg_end - seg >= 16 && matches(d, seg, std::string_view("MPF\0", 4))) {
            MpfIndex idx;
            idx.base = seg + 4;
            if (matches(d, idx.base, std::string_view("II*\0", 4))) idx.little_endian = true;
            else if (!matches(d, idx.base, std::string_view("MM\0*", 4))) return std::nullopt;
            const std::size_t ifd = idx.base + read32(d, idx.base + 4, idx.little_endian);
            if (ifd + 2 > seg_end) return std::nullopt;
            const std::size_t tags = read16(d, ifd, idx.little_endian);
            for (std::size_t i = 0; i < tags; ++i) {
                const std::size_t t = ifd + 2 + 12 * i;
                if (t + 12 > seg_end) return std::nullopt;
                if (read16(d, t, idx.little_endian) != 0xB002) continue;
                idx.count = read32(d, t + 4, idx.little_endian) / 16;
                idx.entries = idx.base + read32(d, t + 8, idx.little_endian);
                if (idx.count == 0 || idx.entries + 16 * idx.count > seg_end) return std::nullopt;
                return idx;
            }
            return std::nullopt;
        }
        p = seg_end;
    }
    return std::nullopt;
}

std::optional<Layout> parse_layout(const std::span<const uint8_t> data) {
    const auto end = first_image_end(data);
    if (!end) return std::nullopt;
    Layout layout;
    layout.primary_end = *end;
    layout.mpf = find_mpf_index(data, *end);
    if (layout.mpf) {
        const MpfIndex& idx = *layout.mpf;
        for (std::size_t k = 1; k < idx.count; ++k) {
            const std::size_t entry = idx.entries + 16 * k;
            const std::size_t size = read32(data, entry + 4, idx.little_endian);
            const std::size_t offset = read32(data, entry + 8, idx.little_endian);
            if (offset == 0) continue;
            const std::size_t start = idx.base + offset;
            if (start < *end || size < 4 || start + size > data.size() || data[start] != 0xFF || data[start + 1] != 0xD8) {
                return std::nullopt;
            }
            layout.images.push_back({k, start, size});
        }
        std::ranges::sort(layout.images, {}, &SecondaryImage::start);
        for (std::size_t i = 1; i < layout.images.size(); ++i) {
            if (layout.images[i].start < layout.images[i - 1].start + layout.images[i - 1].size) return std::nullopt;
        }
    }
    layout.trailing_start = layout.images.empty() ? *end : layout.images.back().start + layout.images.back().size;
    layout.trailers_start = metadata_trailers_start(data, layout.trailing_start);
    return layout;
}

std::size_t metadata_trailers_start(const std::span<const uint8_t> data, const std::size_t from) {
    return walk_trailers(data, from, 0, [](std::size_t, std::size_t) {});
}

bool is_padding(const std::span<const uint8_t> bytes) {
    if (bytes.empty() || (bytes[0] != 0x00 && bytes[0] != 0xFF)) return false;
    return std::ranges::all_of(bytes, [v = bytes[0]](const uint8_t b) { return b == v; });
}

void relocate_trailers(const std::span<uint8_t> block, const std::size_t old_pos, const std::size_t new_pos) {
    const auto shift = static_cast<int64_t>(new_pos) - static_cast<int64_t>(old_pos);
    walk_trailers(block, 0, old_pos, [&](const std::size_t start, const std::size_t end) {
        const bool le = block[end - 9] == '*';
        const auto move = [&](const std::size_t at) {
            write32(block, at, static_cast<uint32_t>(read32(block, at, le) + shift), le);
        };
        const std::size_t dir = start + 12;
        const std::size_t n = read16(block, start + 6, le);
        for (std::size_t i = 0; i < n; ++i) move(dir + 12 * i + 8);
        move(end - 8);
    });
}

std::span<const uint8_t> trailing_content(const std::span<const uint8_t> data, const Layout& layout) {
    const auto body = data.subspan(layout.trailing_start, layout.trailers_start - layout.trailing_start);
    return is_padding(body) ? body.first(0) : body;
}

std::optional<std::string> find_xmp(const std::span<const uint8_t> image, const std::size_t limit) {
    const auto payload = app1_payload(image, limit, kXmpId);
    if (!payload || payload->size() == kXmpId.size()) return std::nullopt;
    return std::string(reinterpret_cast<const char*>(payload->data()) + kXmpId.size(), payload->size() - kXmpId.size());
}

std::optional<std::vector<uint8_t>> find_exif(const std::span<const uint8_t> image, const std::size_t limit) {
    const auto payload = app1_payload(image, limit, kExifId);
    if (!payload) return std::nullopt;
    return std::vector<uint8_t>(payload->begin(), payload->end());
}

bool has_c2pa_manifest(const std::span<const uint8_t> image, const std::size_t limit) {
    std::size_t p = 2;
    while (p + 4 <= limit && image[p] == 0xFF) {
        const uint8_t marker = image[p + 1];
        if (marker == 0xDA || marker == 0xD9) break;
        const std::size_t len = (static_cast<std::size_t>(image[p + 2]) << 8) | image[p + 3];
        if (len < 2 || p + 2 + len > limit) break;
        if (marker == 0xEB) {
            const std::string_view payload(reinterpret_cast<const char*>(image.data() + p + 4), len - 2);
            if (payload.starts_with("JP") && payload.find("c2pa") != std::string_view::npos) return true;
        }
        p += 2 + len;
    }
    return false;
}

bool is_apple_gain_map(const std::span<const uint8_t> image) {
    const auto end = first_image_end(image);
    const auto xmp = find_xmp(image, end ? *end : image.size());
    return xmp && xmp->find("urn:com:apple:photo:2020:aux:hdrgainmap") != std::string::npos;
}

std::optional<std::vector<uint8_t>> apple_hdr_exif(const std::span<const uint8_t> exif) {
    if (exif.size() < kExifId.size() + 8 || !matches(exif, 0, kExifId)) return std::nullopt;
    const auto tiff = exif.subspan(kExifId.size());
    const auto order = byte_order(tiff, 0);
    if (!order) return std::nullopt;
    const auto exif_ptr = ifd_entry(tiff, read32(tiff, 4, *order), 0x8769, *order);
    if (!exif_ptr) return std::nullopt;
    const auto note_entry = ifd_entry(tiff, read32(tiff, *exif_ptr + 8, *order), 0x927C, *order);
    if (!note_entry) return std::nullopt;
    const std::size_t note_size = read32(tiff, *note_entry + 4, *order);
    const std::size_t note_offset = read32(tiff, *note_entry + 8, *order);
    if (note_size < 16 || note_offset > tiff.size() || note_size > tiff.size() - note_offset) return std::nullopt;
    const auto note = tiff.subspan(note_offset, note_size);
    const auto note_order = byte_order(note, 12);
    if (!matches(note, 0, std::string_view("Apple iOS\0", 10)) || !note_order) return std::nullopt;

    // hdr headroom and gain; offsets inside apple's maker note are relative to its own start
    struct Entry { uint16_t tag, type; uint32_t count; std::span<const uint8_t> value; };
    std::vector<Entry> kept;
    for (const uint16_t tag : {uint16_t{0x0021}, uint16_t{0x0030}}) {
        const auto e = ifd_entry(note, 14, tag, *note_order);
        if (!e) continue;
        const uint16_t type = read16(note, *e + 2, *note_order);
        const uint32_t count = read32(note, *e + 4, *note_order);
        const std::size_t size = tiff_type_size(type) * count;
        if (size == 0 || size > 64) continue;
        const std::size_t at = size <= 4 ? *e + 8 : read32(note, *e + 8, *note_order);
        if (at > note.size() || size > note.size() - at) continue;
        kept.push_back({tag, type, count, note.subspan(at, size)});
    }
    if (kept.empty()) return std::nullopt;

    std::vector<uint8_t> mn(note.begin(), note.begin() + 14);
    const bool le = *note_order;
    const std::size_t values_start = 14 + 2 + 12 * kept.size() + 4;
    std::vector<uint8_t> values;
    put16(mn, static_cast<uint16_t>(kept.size()), le);
    for (const auto& k : kept) {
        put16(mn, k.tag, le);
        put16(mn, k.type, le);
        put32(mn, k.count, le);
        if (k.value.size() <= 4) {
            mn.insert(mn.end(), k.value.begin(), k.value.end());
            mn.resize(mn.size() + 4 - k.value.size(), 0);
        } else {
            put32(mn, static_cast<uint32_t>(values_start + values.size()), le);
            values.insert(values.end(), k.value.begin(), k.value.end());
        }
    }
    put32(mn, 0, le);
    mn.insert(mn.end(), values.begin(), values.end());

    // big-endian tiff: ifd0 -> exif ifd -> maker note, nothing else
    std::vector<uint8_t> out(kExifId.begin(), kExifId.end());
    out.insert(out.end(), {'M', 'M', 0x00, 0x2A});
    put32(out, 8, false);
    put16(out, 1, false);
    put16(out, 0x8769, false);
    put16(out, 4, false);
    put32(out, 1, false);
    put32(out, 26, false);
    put32(out, 0, false);
    put16(out, 1, false);
    put16(out, 0x927C, false);
    put16(out, 7, false);
    put32(out, static_cast<uint32_t>(mn.size()), false);
    put32(out, 44, false);
    put32(out, 0, false);
    out.insert(out.end(), mn.begin(), mn.end());
    return out;
}

std::string reduce_xmp(const std::string_view packet) {
    pugi::xml_document src;
    if (!src.load_buffer(packet.data(), packet.size(), pugi::parse_default | pugi::parse_pi)) return {};
    const Namespaces ns = namespaces(src);

    pugi::xml_document out;
    pugi::xml_node head = out.append_child(pugi::node_pi);
    head.set_name("xpacket");
    head.set_value("begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"");
    pugi::xml_node meta = out.append_child("x:xmpmeta");
    meta.append_attribute("xmlns:x") = "adobe:ns:meta/";
    pugi::xml_node rdf = meta.append_child("rdf:RDF");
    rdf.append_attribute("xmlns:rdf") = std::string(kRdfNs).c_str();
    pugi::xml_node desc = rdf.append_child("rdf:Description");
    desc.append_attribute("rdf:about") = "";

    bool kept = false;
    std::set<std::string, std::less<>> used;
    for_each_element(src, [&](const pugi::xml_node e) {
        const std::string_view name = e.name();
        const auto rdf_ns = ns.find(prefix_of(name));
        if (!name.ends_with(":Description") || rdf_ns == ns.end() || rdf_ns->second != kRdfNs) return;
        for (const pugi::xml_attribute a : e.attributes()) {
            if (!is_link_property(a.name(), ns)) continue;
            desc.append_attribute(a.name()) = a.value();
            used.emplace(prefix_of(a.name()));
            kept = true;
        }
        for (pugi::xml_node c = e.first_child(); c; c = c.next_sibling()) {
            if (c.type() != pugi::node_element || !is_link_property(c.name(), ns)) continue;
            desc.append_copy(c);
            collect_prefixes(c, used);
            kept = true;
        }
    });
    if (!kept) return {};

    for (const auto& prefix : used) {
        if (prefix.empty() || prefix == "xml" || prefix == "xmlns") continue;
        const auto it = ns.find(prefix);
        if (it == ns.end()) return {};
        if (prefix == "rdf" && it->second == kRdfNs) continue;
        desc.append_attribute(("xmlns:" + prefix).c_str()) = it->second.c_str();
    }
    pugi::xml_node tail = out.append_child(pugi::node_pi);
    tail.set_name("xpacket");
    tail.set_value("end=\"w\"");
    return serialize(out);
}

std::optional<std::string> set_item_length(const std::string_view packet, const std::string_view semantic,
                                           const uint64_t length) {
    pugi::xml_document doc;
    if (!doc.load_buffer(packet.data(), packet.size(), pugi::parse_full)) return std::nullopt;
    const std::string item = prefix_for(namespaces(doc), kItemNs);
    if (item.empty()) return std::nullopt;
    const std::string semantic_name = item + ":Semantic";
    const std::string length_name = item + ":Length";
    bool found = false;
    for_each_element(doc, [&](pugi::xml_node e) {
        const pugi::xml_attribute s = e.attribute(semantic_name.c_str());
        if (!s || semantic != s.value()) return;
        pugi::xml_attribute l = e.attribute(length_name.c_str());
        if (!l) l = e.append_attribute(length_name.c_str());
        l.set_value(std::to_string(length).c_str());
        found = true;
    });
    if (!found) return std::nullopt;
    return serialize(doc);
}

XmpLinks parse_xmp_links(const std::string_view packet) {
    XmpLinks links;
    pugi::xml_document doc;
    if (!doc.load_buffer(packet.data(), packet.size(), pugi::parse_default)) return links;
    const Namespaces ns = namespaces(doc);
    const std::string camera = prefix_for(ns, kGCameraNs);
    const std::string item = prefix_for(ns, kItemNs);
    for_each_element(doc, [&](const pugi::xml_node e) {
        if (!camera.empty()) {
            const std::string offset_name = camera + ":MicroVideoOffset";
            if (const pugi::xml_attribute a = e.attribute(offset_name.c_str())) {
                links.micro_video_offset = a.as_ullong();
            } else if (offset_name == e.name()) {
                links.micro_video_offset = e.text().as_ullong();
            }
        }
        if (!item.empty()) {
            if (const pugi::xml_attribute s = e.attribute((item + ":Semantic").c_str())) {
                links.items.push_back({s.value(), e.attribute((item + ":Mime").c_str()).value(),
                                       e.attribute((item + ":Length").c_str()).as_ullong()});
            }
        }
    });
    return links;
}

} // namespace chisel::jpeg
