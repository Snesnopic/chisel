//
// Created by Giuseppe Francione on 26/09/26.
//

#include "../../include/jpeg_transcode.hpp"
#include "../../include/jpeg_structure.hpp"
#include "../../include/logger.hpp"
#include <jpeglib.h>
#include <array>
#include <csetjmp>
#include <cstddef>
#include <cstring>
#include <string_view>

namespace chisel::jpeg {

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
 * @brief Tells whether decoders need the source's Adobe segment to read its colors, i.e. whether without
 *        it they would pick another color space from the JFIF header or the component ids.
 */
bool adobe_decides_colors(const j_decompress_ptr srcinfo) {
    if (!srcinfo->saw_Adobe_marker || srcinfo->num_components == 1) return false;
    if (srcinfo->num_components != 3 || srcinfo->Adobe_transform != 1) return true;
    const jpeg_component_info* c = srcinfo->comp_info;
    return !srcinfo->saw_JFIF_marker && c[0].component_id == 'R' && c[1].component_id == 'G' &&
           c[2].component_id == 'B';
}

/**
 * @brief Copies the saved markers allowed by @p policy, plus an Adobe segment that decides the colors, in
 *        their original order from the decompressor to the compressor.
 */
void write_markers(const j_decompress_ptr srcinfo, const j_compress_ptr dstinfo, const MarkerPolicy& policy) {
    const bool keep_adobe = adobe_decides_colors(srcinfo);
    std::vector<MarkerData> markers;
    for (jpeg_saved_marker_ptr m = srcinfo->marker_list; m; m = m->next) {
        if ((m->marker >= JPEG_APP0 && m->marker <= JPEG_APP0 + 15) ||
            m->marker == JPEG_COM) {
            if (m->data && m->data_length > 0) {
                MarkerData md{.marker=m->marker, .data={m->data, m->data + m->data_length}};
                const bool adobe = keep_adobe && md.marker == JPEG_APP0 + 14 && has_prefix(md.data, "Adobe");
                if (adobe || keep_marker(md, policy)) markers.push_back(std::move(md));
            }
        }
    }

    // a segment repeating the previous one of its kind adds nothing
    std::array<const MarkerData*, 17> last{};
    for (const auto& m : markers) {
        const MarkerData*& previous = last[m.marker == JPEG_COM ? 16 : m.marker - JPEG_APP0];
        if (previous && previous->data == m.data) continue;
        previous = &m;
        jpeg_write_marker(dstinfo, m.marker, m.data.data(), static_cast<unsigned int>(m.data.size()));
    }
}

} // namespace

bool transcode(const std::span<const uint8_t> in, const MarkerPolicy& policy, const ScanMode mode,
               std::vector<uint8_t>& out, bool& clean) {
    jpeg_decompress_struct srcinfo{};
    // progressive, then baseline
    std::array<jpeg_compress_struct, 2> dstinfo{};
    std::array<std::vector<uint8_t>, 2> encoded;
    std::array<VectorDest, 2> dest{};
    JpegErrorMgr jsrcerr{}, jdsterr{};

    srcinfo.err = jpeg_std_error(&jsrcerr.pub);
    jsrcerr.pub.error_exit = jpeg_error_exit_longjmp;
    jpeg_std_error(&jdsterr.pub);
    jdsterr.pub.error_exit = jpeg_error_exit_longjmp;
    for (std::size_t i = 0; i < dstinfo.size(); ++i) {
        dstinfo[i].err = &jdsterr.pub;
        dest[i].out = &encoded[i];
        dest[i].pub.init_destination = vector_init;
        dest[i].pub.empty_output_buffer = vector_grow;
        dest[i].pub.term_destination = vector_term;
    }

    if (setjmp(jsrcerr.setjmp_buffer) || setjmp(jdsterr.setjmp_buffer)) {
        for (auto& d : dstinfo) jpeg_destroy_compress(&d);
        jpeg_destroy_decompress(&srcinfo);
        return false;
    }

    jpeg_create_decompress(&srcinfo);
    for (auto& d : dstinfo) jpeg_create_compress(&d);

    jpeg_mem_src(&srcinfo, in.data(), static_cast<unsigned long>(in.size()));
    for (int m = 0; m < 16; ++m) {
        jpeg_save_markers(&srcinfo, JPEG_APP0 + m, 0xFFFF);
    }
    jpeg_save_markers(&srcinfo, JPEG_COM, 0xFFFF);

    if (jpeg_read_header(&srcinfo, TRUE) != JPEG_HEADER_OK) {
        for (auto& d : dstinfo) jpeg_destroy_compress(&d);
        jpeg_destroy_decompress(&srcinfo);
        return false;
    }

    Logger::log(LogLevel::Debug,
                std::string("Jpeg ") + (srcinfo.progressive_mode ? "progressive" : "baseline"), "libjpeg");
    // a container that declares the scan layout keeps it
    const std::array<bool, 2> wanted = {mode == ScanMode::Smallest || srcinfo.progressive_mode,
                                        mode == ScanMode::Smallest || !srcinfo.progressive_mode};

    jvirt_barray_ptr *coef_arrays = jpeg_read_coefficients(&srcinfo);
    clean = srcinfo.src->bytes_in_buffer == 0 && jsrcerr.pub.num_warnings == 0;
    for (std::size_t i = 0; i < dstinfo.size(); ++i) {
        if (!wanted[i]) continue;
        const bool baseline = i == 1;
        // libjpeg's own defaults: a single sequential scan
        if (baseline) jpeg_c_set_int_param(&dstinfo[i], JINT_COMPRESS_PROFILE, JCP_FASTEST);
        jpeg_copy_critical_parameters(&srcinfo, &dstinfo[i]);
        // the source's own jfif and adobe segments are copied with the rest; without metadata a bare jfif stands in
        dstinfo[i].write_JFIF_header = !policy.keep_all && srcinfo.saw_JFIF_marker;
        dstinfo[i].write_Adobe_marker = FALSE;
        if (!baseline && srcinfo.progressive_mode) {
            jpeg_simple_progression(&dstinfo[i]);
        }
        dstinfo[i].optimize_coding = TRUE;
        dstinfo[i].dest = &dest[i].pub;
        jpeg_write_coefficients(&dstinfo[i], coef_arrays);
        write_markers(&srcinfo, &dstinfo[i], policy);
        jpeg_finish_compress(&dstinfo[i]);
    }
    // Do NOT call jpeg_finish_decompress(&srcinfo) when using jpeg_read_coefficients
    for (auto& d : dstinfo) jpeg_destroy_compress(&d);
    jpeg_destroy_decompress(&srcinfo);
    const bool pick_baseline = !wanted[0] || (wanted[1] && encoded[1].size() < encoded[0].size());
    out = std::move(pick_baseline ? encoded[1] : encoded[0]);
    return true;
}

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

} // namespace chisel::jpeg
