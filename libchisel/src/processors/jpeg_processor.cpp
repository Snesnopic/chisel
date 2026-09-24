//
// Created by Giuseppe Francione on 19/10/25.
//

#include "../../include/jpeg_processor.hpp"
#include "../../include/jpeg_structure.hpp"
#include "../../include/logger.hpp"
#include <jpeglib.h>
#include <cstdio>
#include <stdexcept>
#include <vector>
#include <algorithm>
#include <memory>
#include <csetjmp>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <optional>
#include <span>
#include "file_utils.hpp"
namespace chisel {

namespace {

// error manager (jpeg error -> c++ exception)
struct JpegErrorMgr {
    jpeg_error_mgr pub{};
    char msg[JMSG_LENGTH_MAX]{};
    jmp_buf setjmp_buffer;
};

/**
 * @brief libjpeg error handler that jumps back on error.
 * @param cinfo Pointer to the libjpeg error context.
 */
void jpeg_error_exit_longjmp(const j_common_ptr cinfo) {
    auto *err = reinterpret_cast<JpegErrorMgr *>(cinfo->err);
    (*cinfo->err->format_message)(cinfo, err->msg);
    Logger::log(LogLevel::Warning, std::string("Libjpeg: ") + err->msg, "libjpeg");
    longjmp(err->setjmp_buffer, 1);
}

// libjpeg destination that grows a std::vector
struct VectorDest {
    jpeg_destination_mgr pub{};
    std::vector<uint8_t>* out = nullptr;
};

void vector_init(const j_compress_ptr cinfo) {
    auto* dest = reinterpret_cast<VectorDest*>(cinfo->dest);
    dest->out->resize(1 << 16);
    dest->pub.next_output_byte = dest->out->data();
    dest->pub.free_in_buffer = dest->out->size();
}

boolean vector_grow(const j_compress_ptr cinfo) {
    auto* dest = reinterpret_cast<VectorDest*>(cinfo->dest);
    const std::size_t used = dest->out->size();
    dest->out->resize(used * 2);
    dest->pub.next_output_byte = dest->out->data() + used;
    dest->pub.free_in_buffer = dest->out->size() - used;
    return TRUE;
}

void vector_term(const j_compress_ptr cinfo) {
    auto* dest = reinterpret_cast<VectorDest*>(cinfo->dest);
    dest->out->resize(dest->out->size() - dest->pub.free_in_buffer);
}

/**
 * @brief Which markers of an image survive re-encoding.
 */
struct MarkerPolicy {
    bool keep_all = true;                    ///< preserve metadata
    bool keep_links = false;                 ///< without metadata, still keep MPF and ISO gain map segments
    std::optional<std::string> xmp;          ///< replacement standard XMP packet; empty drops it
    std::optional<std::vector<uint8_t>> exif; ///< replacement EXIF payload
};

bool has_prefix(const std::vector<JOCTET>& data, const std::string_view id) {
    return data.size() >= id.size() && memcmp(data.data(), id.data(), id.size()) == 0;
}

struct MarkerData {
    int marker;
    std::vector<JOCTET> data;
};

bool keep_marker(MarkerData& m, const MarkerPolicy& policy) {
    if (m.marker == JPEG_APP0 + 1 && has_prefix(m.data, jpeg::kXmpId)) {
        if (!policy.xmp) return policy.keep_all;
        if (policy.xmp->empty()) return false;
        m.data.assign(jpeg::kXmpId.begin(), jpeg::kXmpId.end());
        m.data.insert(m.data.end(), policy.xmp->begin(), policy.xmp->end());
        return true;
    }
    if (m.marker == JPEG_APP0 + 1 && has_prefix(m.data, jpeg::kExifId) && policy.exif) {
        m.data.assign(policy.exif->begin(), policy.exif->end());
        return true;
    }
    if (policy.keep_all) return true;
    if (m.marker != JPEG_APP0 + 2) return false;
    // the color profile is part of how the image looks, not metadata
    if (has_prefix(m.data, std::string_view("ICC_PROFILE\0", 12))) return true;
    return policy.keep_links && (has_prefix(m.data, std::string_view("MPF\0", 4)) ||
                                 has_prefix(m.data, std::string_view("urn:iso:std:iso:ts:21496:-1\0", 28)));
}

/**
 * @brief Copies the saved markers allowed by @p policy from the decompressor to the compressor.
 */
void write_markers(const j_decompress_ptr srcinfo, const j_compress_ptr dstinfo, const MarkerPolicy& policy) {
    std::vector<MarkerData> markers;
    for (jpeg_saved_marker_ptr m = srcinfo->marker_list; m; m = m->next) {
        if ((m->marker >= JPEG_APP0 && m->marker <= JPEG_APP0 + 15) ||
            m->marker == JPEG_COM) {
            if (m->data && m->data_length > 0) {
                MarkerData md{.marker=m->marker, .data={m->data, m->data + m->data_length}};
                if (keep_marker(md, policy)) markers.push_back(std::move(md));
            }
        }
    }

    std::ranges::stable_sort(markers,
                      [](const auto &a, const auto &b) { return a.marker < b.marker; });

    markers.erase(std::unique(markers.begin(), markers.end(),
                              [](const auto &a, const auto &b) {
                                  return a.marker == b.marker && a.data == b.data;
                              }),
                  markers.end());

    for (const auto &m: markers) {
        // jpeg_write_coefficients() already wrote its own JFIF/Adobe header; skip the source's to avoid a duplicate
        if (dstinfo->write_JFIF_header && m.marker == JPEG_APP0 &&
            m.data.size() >= 5 && memcmp(m.data.data(), "JFIF\0", 5) == 0) {
            continue;
        }
        if (dstinfo->write_Adobe_marker && m.marker == JPEG_APP0 + 14 &&
            m.data.size() >= 5 && memcmp(m.data.data(), "Adobe", 5) == 0) {
            continue;
        }
        jpeg_write_marker(dstinfo, m.marker, m.data.data(), static_cast<unsigned int>(m.data.size()));
    }
}

/**
 * @brief Losslessly re-encodes one JPEG image held in memory.
 * @param mirror_jfif Write a JFIF header only if the source had one.
 * @param clean Set when libjpeg ended the image exactly at the end of @p in, without warnings.
 * @return False on a libjpeg error.
 */
bool transcode(const std::span<const uint8_t> in, const MarkerPolicy& policy, const bool mirror_jfif,
               std::vector<uint8_t>& out, bool& clean) {
    jpeg_decompress_struct srcinfo{};
    jpeg_compress_struct dstinfo{};
    JpegErrorMgr jsrcerr{}, jdsterr{};
    VectorDest dest{};
    dest.out = &out;
    dest.pub.init_destination = vector_init;
    dest.pub.empty_output_buffer = vector_grow;
    dest.pub.term_destination = vector_term;

    srcinfo.err = jpeg_std_error(&jsrcerr.pub);
    jsrcerr.pub.error_exit = jpeg_error_exit_longjmp;
    dstinfo.err = jpeg_std_error(&jdsterr.pub);
    jdsterr.pub.error_exit = jpeg_error_exit_longjmp;

    if (setjmp(jsrcerr.setjmp_buffer) || setjmp(jdsterr.setjmp_buffer)) {
        jpeg_destroy_compress(&dstinfo);
        jpeg_destroy_decompress(&srcinfo);
        return false;
    }

    jpeg_create_decompress(&srcinfo);
    jpeg_create_compress(&dstinfo);

    jpeg_mem_src(&srcinfo, in.data(), static_cast<unsigned long>(in.size()));
    for (int m = 0; m < 16; ++m) {
        jpeg_save_markers(&srcinfo, JPEG_APP0 + m, 0xFFFF);
    }
    jpeg_save_markers(&srcinfo, JPEG_COM, 0xFFFF);

    if (jpeg_read_header(&srcinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_compress(&dstinfo);
        jpeg_destroy_decompress(&srcinfo);
        return false;
    }

    Logger::log(LogLevel::Debug,
                std::string("Jpeg ") + (srcinfo.progressive_mode ? "progressive" : "baseline"),
                "JpegProcessor");

    jvirt_barray_ptr *coef_arrays = jpeg_read_coefficients(&srcinfo);
    clean = srcinfo.src->bytes_in_buffer == 0 && jsrcerr.pub.num_warnings == 0;
    jpeg_copy_critical_parameters(&srcinfo, &dstinfo);
    if (mirror_jfif) {
        dstinfo.write_JFIF_header = srcinfo.saw_JFIF_marker;
    }

    if (srcinfo.progressive_mode) {
        jpeg_simple_progression(&dstinfo);
    }

    dstinfo.optimize_coding = TRUE;
    dstinfo.dest = &dest.pub;
    jpeg_write_coefficients(&dstinfo, coef_arrays);
    write_markers(&srcinfo, &dstinfo, policy);
    jpeg_finish_compress(&dstinfo);
    // Do NOT call jpeg_finish_decompress(&srcinfo) when using jpeg_read_coefficients
    jpeg_destroy_compress(&dstinfo);
    jpeg_destroy_decompress(&srcinfo);
    return true;
}

struct Pixels {
    int width = 0;
    int height = 0;
    int channels = 0;
    std::vector<unsigned char> data;
    bool operator==(const Pixels&) const = default;
};

/**
 * @brief Decodes the first image of a JPEG held in memory.
 * @return False if it can't be decoded.
 */
bool decode_pixels(const std::span<const uint8_t> in, Pixels& px) {
    jpeg_decompress_struct cinfo{};
    JpegErrorMgr jsrcerr{};
    cinfo.err = jpeg_std_error(&jsrcerr.pub);
    jsrcerr.pub.error_exit = jpeg_error_exit_longjmp;

    if (setjmp(jsrcerr.setjmp_buffer)) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, in.data(), static_cast<unsigned long>(in.size()));
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    jpeg_start_decompress(&cinfo);
    px.width = static_cast<int>(cinfo.output_width);
    px.height = static_cast<int>(cinfo.output_height);
    px.channels = cinfo.output_components;
    const std::size_t stride = static_cast<std::size_t>(cinfo.output_width) * static_cast<std::size_t>(cinfo.output_components);
    px.data.resize(stride * cinfo.output_height);
    while (cinfo.output_scanline < cinfo.output_height) {
        unsigned char* row = px.data.data() + stride * cinfo.output_scanline;
        jpeg_read_scanlines(&cinfo, &row, 1);
    }
    // Do not call jpeg_finish_decompress when we break early or just want to destroy
    jpeg_destroy_decompress(&cinfo);
    return true;
}

/**
 * @brief Writes new MPF sizes and offsets into the re-encoded first image.
 * @param starts Absolute offsets the secondary images will have in the output.
 * @return False if the output's MPF data doesn't match the original's.
 */
bool patch_mpf(std::vector<uint8_t>& primary, const jpeg::Layout& layout,
               const std::vector<std::vector<uint8_t>>& images, const std::vector<std::size_t>& starts) {
    if (!layout.mpf) return true;
    const auto idx = jpeg::find_mpf_index(primary, primary.size());
    if (!idx) return layout.images.empty();
    if (idx->count != layout.mpf->count) return false;
    const bool le = idx->little_endian;

    // shift the first image's size by the same delta instead of recomputing it, keeping any writer quirk
    const int64_t size0 = static_cast<int64_t>(jpeg::read32(primary, idx->entries + 4, le)) +
                          static_cast<int64_t>(primary.size()) - static_cast<int64_t>(layout.primary_end);
    if (size0 <= 0 || size0 > UINT32_MAX) return false;
    jpeg::write32(primary, idx->entries + 4, static_cast<uint32_t>(size0), le);

    for (std::size_t i = 0; i < layout.images.size(); ++i) {
        const std::size_t entry = idx->entries + 16 * layout.images[i].entry;
        if (starts[i] < idx->base || starts[i] - idx->base > UINT32_MAX) return false;
        jpeg::write32(primary, entry + 4, static_cast<uint32_t>(images[i].size()), le);
        jpeg::write32(primary, entry + 8, static_cast<uint32_t>(starts[i] - idx->base), le);
    }
    return true;
}

/**
 * @brief Re-encodes the first image and every MPF secondary image, then reattaches what follows.
 *
 * Without metadata, IPTC trailers and EXIF go away; padding always does. Anything located
 * from the end of the file (motion photo video) keeps its position relative to the end.
 */
std::vector<uint8_t> rebuild(const std::span<const uint8_t> data, const jpeg::Layout& layout,
                             const bool preserve_metadata, const std::string& name) {
    const auto primary = data.first(layout.primary_end);
    const auto body = data.subspan(layout.trailing_start, layout.trailers_start - layout.trailing_start);
    const auto trailers = data.subspan(layout.trailers_start);
    const auto xmp = jpeg::find_xmp(primary, primary.size());
    const jpeg::XmpLinks links = xmp ? jpeg::parse_xmp_links(*xmp) : jpeg::XmpLinks{};

    const bool end_relative = links.micro_video_offset.has_value() ||
        std::ranges::any_of(links.items, [](const jpeg::ContainerItem& i) { return i.length > 0 && i.semantic != "GainMap"; });
    const bool keep_body = !body.empty() && (end_relative || !jpeg::is_padding(body));
    const bool keep_trailers = !trailers.empty() && (preserve_metadata || end_relative);
    const bool has_links = !layout.images.empty() || keep_body;
    const bool declares_gain_map =
        std::ranges::any_of(links.items, [](const jpeg::ContainerItem& i) { return i.semantic == "GainMap"; });

    if (layout.primary_end < data.size()) {
        Logger::log(LogLevel::Debug, "Rebuilding " + std::to_string(layout.images.size()) + " secondary images and " +
                    std::to_string(data.size() - layout.trailing_start) + " trailing bytes", "JpegProcessor");
    }

    // secondary images first: their new sizes go into the first image's MPF and XMP
    std::vector<std::vector<uint8_t>> images;
    for (const auto& img : layout.images) {
        const auto src = data.subspan(img.start, img.size);
        MarkerPolicy policy{.keep_all = preserve_metadata, .keep_links = true, .xmp = std::nullopt, .exif = std::nullopt};
        if (!preserve_metadata) {
            if (const auto x = jpeg::find_xmp(src, src.size())) policy.xmp = jpeg::reduce_xmp(*x);
        }
        std::vector<uint8_t> out;
        bool clean = false;
        const bool resizable = !declares_gain_map || layout.images.size() == 1;
        if (resizable && jpeg::first_image_end(src) == src.size() &&
            transcode(src, policy, true, out, clean) && clean && out.size() < src.size()) {
            images.push_back(std::move(out));
        } else {
            images.emplace_back(src.begin(), src.end());
        }
    }
    const auto restore_image = [&](const std::size_t i) {
        const auto src = data.subspan(layout.images[i].start, layout.images[i].size);
        images[i].assign(src.begin(), src.end());
    };

    MarkerPolicy policy{.keep_all = preserve_metadata, .keep_links = has_links, .xmp = std::nullopt, .exif = std::nullopt};
    // apple renders the gain map using maker note values from this exif, so keep just those
    const bool apple_hdr = std::ranges::any_of(layout.images, [&](const jpeg::SecondaryImage& img) {
        return jpeg::is_apple_gain_map(data.subspan(img.start, img.size));
    });
    if (!preserve_metadata && apple_hdr) {
        if (const auto exif = jpeg::find_exif(primary, primary.size())) {
            policy.exif = jpeg::apple_hdr_exif(*exif);
        }
    }
    if (xmp) {
        std::string packet = *xmp;
        bool updated = false;
        if (declares_gain_map && images.size() == 1 && images[0].size() != layout.images[0].size) {
            if (auto p = jpeg::set_item_length(packet, "GainMap", images[0].size());
                p && p->size() + jpeg::kXmpId.size() <= 65533) {
                packet = std::move(*p);
                updated = true;
            } else {
                restore_image(0);
            }
        }
        if (!preserve_metadata) {
            policy.xmp = has_links ? jpeg::reduce_xmp(packet) : std::string{};
        } else if (updated) {
            policy.xmp = std::move(packet);
        }
    }

    std::vector<uint8_t> out;
    bool clean = false;
    if (!transcode(primary, policy, false, out, clean)) {
        Logger::log(LogLevel::Error, "Recompression failed due to libjpeg error", "JpegProcessor");
        throw std::runtime_error("Libjpeg error");
    }
    if (layout.primary_end < data.size() && !clean) {
        Logger::log(LogLevel::Warning, "First image didn't end cleanly, leaving " + name + " unchanged", "JpegProcessor");
        return {data.begin(), data.end()};
    }

    // gaps between secondary images are copied as they are, so only the images move
    std::vector<std::size_t> starts;
    std::size_t pos = out.size();
    std::size_t cursor = layout.primary_end;
    for (std::size_t i = 0; i < layout.images.size(); ++i) {
        pos += layout.images[i].start - cursor;
        starts.push_back(pos);
        pos += images[i].size();
        cursor = layout.images[i].start + layout.images[i].size;
    }
    if (!patch_mpf(out, layout, images, starts)) {
        Logger::log(LogLevel::Warning, "Inconsistent MPF data, leaving " + name + " unchanged", "JpegProcessor");
        return {data.begin(), data.end()};
    }

    cursor = layout.primary_end;
    for (std::size_t i = 0; i < layout.images.size(); ++i) {
        out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(cursor),
                   data.begin() + static_cast<std::ptrdiff_t>(layout.images[i].start));
        out.insert(out.end(), images[i].begin(), images[i].end());
        cursor = layout.images[i].start + layout.images[i].size;
    }
    if (keep_body) {
        out.insert(out.end(), body.begin(), body.end());
    }
    if (keep_trailers) {
        const std::size_t at = out.size();
        out.insert(out.end(), trailers.begin(), trailers.end());
        jpeg::relocate_trailers(std::span(out).subspan(at), layout.trailers_start, at);
    }
    return out;
}

} // namespace

void JpegProcessor::recompress(const std::filesystem::path& input,
                               const std::filesystem::path& output, const ProcessingOptions &options) {
    Logger::log(LogLevel::Debug, "Entering recompress for " + input.string(), get_name());

    std::vector<uint8_t> data;
    if (!chisel::read_file(input, data)) {
        Logger::log(LogLevel::Error, "Cannot open jpeg input: " + input.string(), get_name());
        throw std::runtime_error("Cannot open JPEG input");
    }

    std::vector<uint8_t> result;
    if (const auto layout = jpeg::parse_layout(data)) {
        result = rebuild(data, *layout, options.preserve_metadata, input.filename().string());
    } else if (jpeg::first_image_end(data)) {
        Logger::log(LogLevel::Warning, "Inconsistent MPF data, leaving " + input.filename().string() + " unchanged",
                    get_name());
        result = data;
    } else {
        // malformed marker structure: let libjpeg decide what it can read, as before
        bool clean = false;
        const MarkerPolicy policy{.keep_all = options.preserve_metadata, .keep_links = false, .xmp = std::nullopt,
                                  .exif = std::nullopt};
        if (!transcode(data, policy, false, result, clean)) {
            Logger::log(LogLevel::Error, "Recompression failed due to libjpeg error", get_name());
            throw std::runtime_error("Libjpeg error");
        }
    }

    std::ofstream out(output, std::ios::binary | std::ios::trunc);
    if (!out) {
        Logger::log(LogLevel::Error, "Cannot open jpeg output: " + output.string(), get_name());
        throw std::runtime_error("Cannot open JPEG output");
    }
    out.write(reinterpret_cast<const char*>(result.data()), static_cast<std::streamsize>(result.size()));
    if (!out) {
        Logger::log(LogLevel::Error, "Cannot write jpeg output: " + output.string(), get_name());
        throw std::runtime_error("Cannot write JPEG output");
    }

    Logger::log(LogLevel::Debug, "Exiting recompress for " + output.string(), get_name());
}

bool JpegProcessor::raw_equal(const std::filesystem::path &a,
                              const std::filesystem::path &b) const {
    std::vector<uint8_t> da, db;
    if (!chisel::read_file(a, da) || !chisel::read_file(b, db)) {
        return false;
    }
    Pixels pa, pb;
    if (!decode_pixels(da, pa) || !decode_pixels(db, pb) || pa != pb) {
        return false;
    }

    const auto la = jpeg::parse_layout(da);
    if (!la) {
        return !jpeg::first_image_end(da) || da == db;
    }
    const auto lb = jpeg::parse_layout(db);
    if (!lb || la->images.size() != lb->images.size()) {
        return false;
    }

    // secondary images must decode to the same pixels; the bytes between them must not change
    const std::span<const uint8_t> sa(da), sb(db);
    std::size_t ca = la->primary_end, cb = lb->primary_end;
    for (std::size_t i = 0; i < la->images.size(); ++i) {
        const auto& x = la->images[i];
        const auto& y = lb->images[i];
        if (x.entry != y.entry || !std::ranges::equal(sa.subspan(ca, x.start - ca), sb.subspan(cb, y.start - cb))) {
            return false;
        }
        Pixels px, py;
        if (!decode_pixels(sa.subspan(x.start, x.size), px) || !decode_pixels(sb.subspan(y.start, y.size), py) ||
            px != py) {
            return false;
        }
        ca = x.start + x.size;
        cb = y.start + y.size;
    }
    if (!std::ranges::equal(jpeg::trailing_content(da, *la), jpeg::trailing_content(db, *lb))) {
        return false;
    }

    // media located from the end of the file must still be where the output's own xmp says
    const auto xa = jpeg::find_xmp(da, la->primary_end);
    if (!xa) {
        return true;
    }
    const auto links_a = jpeg::parse_xmp_links(*xa);
    const auto xb = jpeg::find_xmp(db, lb->primary_end);
    const auto links_b = xb ? jpeg::parse_xmp_links(*xb) : jpeg::XmpLinks{};
    const auto same_end = [&](const uint64_t na, const uint64_t nb) {
        return na <= da.size() && nb <= db.size() && std::ranges::equal(sa.last(na), sb.last(nb));
    };
    if (links_a.micro_video_offset &&
        (!links_b.micro_video_offset || !same_end(*links_a.micro_video_offset, *links_b.micro_video_offset))) {
        return false;
    }
    if (!links_a.items.empty() && links_a.items.back().length > 0 && links_a.items.back().semantic != "GainMap" &&
        (links_b.items.empty() || !same_end(links_a.items.back().length, links_b.items.back().length))) {
        return false;
    }
    return true;
}
std::string JpegProcessor::get_raw_checksum(const std::filesystem::path&) const {
    // TODO: implement checksum of raw JPEG data
    return "";
}

} // namespace chisel
