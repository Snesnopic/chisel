//
// Created by Giuseppe Francione on 19/10/25.
//

#include "../../include/jpeg_processor.hpp"
#include "../../include/jpeg_structure.hpp"
#include "../../include/jpeg_transcode.hpp"
#include "../../include/logger.hpp"
#include <stdexcept>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <optional>
#include <span>
#include "file_utils.hpp"
namespace chisel {

namespace {

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
        jpeg::MarkerPolicy policy{.keep_all = preserve_metadata, .keep_links = true, .xmp = std::nullopt, .exif = std::nullopt};
        if (!preserve_metadata) {
            if (const auto x = jpeg::find_xmp(src, src.size())) policy.xmp = jpeg::reduce_xmp(*x);
        }
        std::vector<uint8_t> out;
        bool clean = false;
        const bool resizable = !declares_gain_map || layout.images.size() == 1;
        if (resizable && jpeg::first_image_end(src) == src.size() &&
            jpeg::transcode(src, policy, jpeg::ScanMode::Smallest, out, clean) && clean && out.size() < src.size()) {
            images.push_back(std::move(out));
        } else {
            images.emplace_back(src.begin(), src.end());
        }
    }
    const auto restore_image = [&](const std::size_t i) {
        const auto src = data.subspan(layout.images[i].start, layout.images[i].size);
        images[i].assign(src.begin(), src.end());
    };

    jpeg::MarkerPolicy policy{.keep_all = preserve_metadata, .keep_links = has_links, .xmp = std::nullopt, .exif = std::nullopt};
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
    if (!jpeg::transcode(primary, policy, jpeg::ScanMode::Smallest, out, clean)) {
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
        const jpeg::MarkerPolicy policy{.keep_all = options.preserve_metadata, .keep_links = false, .xmp = std::nullopt,
                                  .exif = std::nullopt};
        if (!jpeg::transcode(data, policy, jpeg::ScanMode::Smallest, result, clean)) {
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
    out.close();
    if (out.fail()) {
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
    jpeg::Pixels pa, pb;
    if (!jpeg::decode_pixels(da, pa) || !jpeg::decode_pixels(db, pb) || pa != pb) {
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
        jpeg::Pixels px, py;
        if (!jpeg::decode_pixels(sa.subspan(x.start, x.size), px) ||
            !jpeg::decode_pixels(sb.subspan(y.start, y.size), py) ||
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

bool JpegProcessor::is_signed(const std::filesystem::path& file_path) const {
    std::vector<uint8_t> data;
    return read_file(file_path, data) && data.size() > 4 && jpeg::has_c2pa_manifest(data, data.size());
}

} // namespace chisel
