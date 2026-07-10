//
// Created by Giuseppe Francione on 26/03/26.
//

#include "../../include/ico_processor.hpp"
#include "../../include/logger.hpp"
#include "../../include/random_utils.hpp"
#include "../../include/file_utils.hpp"
#include <cstring>
#include <fstream>
#include <vector>
#include <stdexcept>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <memory>
#include <any>


namespace {

// ICO/CUR DIB entries store biHeight as *double* the true image height: the
// first half is the XOR (color) data, the second half is a 1bpp AND mask.
// A plain BMP reader (biHeight taken literally) has no idea about this and
// will expect twice as many color rows as actually exist, running past the
// real pixel data into the mask bytes -- so a naive "wrap the DIB in a BMP
// file header" conversion is only valid for entries where we account for it.
struct IcoEntryMeta {
    enum class Kind { Png, BmpFixedUp, Opaque } kind = Kind::Opaque;
    std::vector<uint8_t> trailing;    // BmpFixedUp: AND mask + padding after the color data
    std::vector<uint8_t> raw_payload; // Opaque: original bytes, re-embedded verbatim
};

uint32_t bmp_row_size(const uint32_t width, const uint32_t bitcount) {
    return ((width * bitcount + 31) / 32) * 4;
}

} // namespace

namespace chisel {

std::optional<ExtractedContent> IcoProcessor::prepare_extraction(const std::filesystem::path& input_path) {
    Logger::log(LogLevel::Debug, "STARTING ICO EXTRACTION FOR " + input_path.string(), get_name());

    ExtractedContent content;
    content.original_path = input_path;
    content.temp_dir = make_temp_dir_for(input_path, "ico");

    const auto data = read_file(input_path);
    if (data.size() < 6) throw std::runtime_error("FILE TOO SMALL TO BE ICO/CUR");

    const uint16_t reserved = read_le16(data.data());
    const uint16_t type = read_le16(data.data() + 2);
    const uint16_t count = read_le16(data.data() + 4);

    if (reserved != 0 || (type != 1 && type != 2)) {
        throw std::runtime_error("INVALID ICO/CUR HEADER");
    }
    if (data.size() < 6 + static_cast<std::size_t>(count) * 16) {
        throw std::runtime_error("TRUNCATED ICO/CUR DIRECTORY");
    }

    const uint8_t png_magic[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    auto metas = std::make_shared<std::vector<IcoEntryMeta>>();

    for (uint16_t i = 0; i < count; ++i) {
        const size_t dir_offset = 6 + i * 16;
        const uint32_t bytes_in_res = read_le32(data.data() + dir_offset + 8);
        const uint32_t image_offset = read_le32(data.data() + dir_offset + 12);

        if (image_offset + bytes_in_res > data.size() || bytes_in_res == 0) {
            Logger::log(LogLevel::Warning, "SKIPPING CORRUPT ICO ENTRY " + std::to_string(i), get_name());
            continue;
        }

        const uint8_t* payload = data.data() + image_offset;
        bool is_png = (bytes_in_res > 8 && std::memcmp(payload, png_magic, 8) == 0);

        IcoEntryMeta meta;
        std::string ext;

        if (is_png) {
            meta.kind = IcoEntryMeta::Kind::Png;
            ext = ".png";
        } else if (bytes_in_res < 40) {
            Logger::log(LogLevel::Warning, "Invalid BMP payload in ICO (too small)", get_name());
            meta.kind = IcoEntryMeta::Kind::Opaque;
            meta.raw_payload.assign(payload, payload + bytes_in_res);
            ext = ".bin";
        } else {
            const uint32_t biSize = read_le32(payload);
            const uint32_t width = read_le32(payload + 4);
            const uint32_t doubled_height = read_le32(payload + 8);
            const uint16_t biBitCount = read_le16(payload + 14);
            const uint32_t biClrUsed = read_le32(payload + 32);

            uint32_t palette_colors = 0;
            if (biBitCount <= 8) {
                palette_colors = (biClrUsed == 0) ? (1u << biBitCount) : biClrUsed;
            }
            const uint32_t palette_size = palette_colors * 4;
            const uint32_t true_height = doubled_height / 2;
            const uint32_t row_size = bmp_row_size(width, biBitCount);
            const uint64_t xor_data_size = static_cast<uint64_t>(row_size) * true_height;

            // sanity check: only attempt the fix-up if the math is internally
            // consistent (even height, non-zero dimensions, color data actually
            // fits within the payload); otherwise this isn't the layout we
            // expect, and guessing further risks corrupting it instead
            const bool plausible = doubled_height != 0 && (doubled_height % 2 == 0) &&
                width != 0 && biBitCount != 0 &&
                (static_cast<uint64_t>(biSize) + palette_size + xor_data_size <= bytes_in_res);

            if (!plausible) {
                Logger::log(LogLevel::Warning,
                    "ICO entry " + std::to_string(i) + " has an unexpected DIB layout, leaving it untouched",
                    get_name());
                meta.kind = IcoEntryMeta::Kind::Opaque;
                meta.raw_payload.assign(payload, payload + bytes_in_res);
                ext = ".bin";
            } else {
                meta.kind = IcoEntryMeta::Kind::BmpFixedUp;
                const uint64_t color_data_end = biSize + palette_size + xor_data_size;
                meta.trailing.assign(payload + color_data_end, payload + bytes_in_res);
                // kept as a fallback in finalize_extraction, in case the pipeline's
                // optimization ends up using a compression method that isn't safe
                // to embed in an ICO/CUR container (see the check there)
                meta.raw_payload.assign(payload, payload + bytes_in_res);
                ext = ".bmp";
            }
        }

        std::filesystem::path out_path = content.temp_dir / (format_index(i) + ext);
        std::ofstream out_file(out_path, std::ios::binary);
        if (!out_file) throw std::runtime_error("CANNOT CREATE FILE: " + out_path.string());

        if (meta.kind == IcoEntryMeta::Kind::Png) {
            out_file.write(reinterpret_cast<const char*>(payload), bytes_in_res);
        } else if (meta.kind == IcoEntryMeta::Kind::Opaque) {
            out_file.write(reinterpret_cast<const char*>(meta.raw_payload.data()), static_cast<std::streamsize>(meta.raw_payload.size()));
        } else {
            // rebuild a standalone BMP: file header + DIB header (with biHeight
            // corrected to the true, un-doubled value) + color data only,
            // omitting the AND mask/padding preserved separately in meta.trailing
            const uint32_t biSize = read_le32(payload);
            const uint32_t width = read_le32(payload + 4);
            const uint32_t true_height = read_le32(payload + 8) / 2;
            const uint16_t biBitCount = read_le16(payload + 14);
            const uint32_t biClrUsed = read_le32(payload + 32);
            uint32_t palette_colors = 0;
            if (biBitCount <= 8) palette_colors = (biClrUsed == 0) ? (1u << biBitCount) : biClrUsed;
            const uint32_t palette_size = palette_colors * 4;
            const uint32_t row_size = bmp_row_size(width, biBitCount);
            const uint32_t xor_data_size = row_size * true_height;

            std::vector<uint8_t> dib_header(payload, payload + biSize);
            write_le32(dib_header.data() + 8, true_height);
            if (biSize >= 24) { // biSizeImage field, if present
                if (read_le32(dib_header.data() + 20) != 0) {
                    write_le32(dib_header.data() + 20, xor_data_size);
                }
            }

            const uint32_t pixel_offset = 14 + biSize + palette_size;
            const uint32_t file_size = pixel_offset + xor_data_size;

            uint8_t bmp_header[14] = {0};
            bmp_header[0] = 'B';
            bmp_header[1] = 'M';
            write_le32(bmp_header + 2, file_size);
            write_le32(bmp_header + 10, pixel_offset);

            out_file.write(reinterpret_cast<const char*>(bmp_header), 14);
            out_file.write(reinterpret_cast<const char*>(dib_header.data()), static_cast<std::streamsize>(dib_header.size()));
            out_file.write(reinterpret_cast<const char*>(payload + biSize), static_cast<std::streamsize>(palette_size));
            out_file.write(reinterpret_cast<const char*>(payload + biSize + palette_size), xor_data_size);
        }
        out_file.close();

        content.extracted_files.push_back(out_path);
        metas->push_back(std::move(meta));
    }

    content.extras = metas;
    content.format = ContainerFormat::Unknown;
    return content;
}

std::filesystem::path IcoProcessor::finalize_extraction(const ExtractedContent& content, const ProcessingOptions&) {
    Logger::log(LogLevel::Debug, "STARTING ICO FINALIZATION FOR " + content.original_path.string(), get_name());

    const auto orig_data = read_file(content.original_path);
    if (orig_data.size() < 6) throw std::runtime_error("ORIGINAL ICO CORRUPTED");

    const uint16_t count = read_le16(orig_data.data() + 4);

    // gather optimized files
    std::vector<std::filesystem::path> optimized_files;
    for (const auto& p : std::filesystem::directory_iterator(content.temp_dir)) {
        if (p.is_regular_file()) optimized_files.push_back(p.path());
    }

    // ensure order matches original index
    std::sort(optimized_files.begin(), optimized_files.end());

    if (optimized_files.size() != count) {
        throw std::runtime_error("MISMATCH BETWEEN EXTRACTED FILES AND ICO DIRECTORY");
    }

    std::shared_ptr<std::vector<IcoEntryMeta>> metas;
    if (content.extras.has_value()) {
        metas = std::any_cast<std::shared_ptr<std::vector<IcoEntryMeta>>>(content.extras);
    }
    if (!metas || metas->size() != count) {
        throw std::runtime_error("MISSING OR MISMATCHED ICO ENTRY METADATA");
    }

    std::vector<uint8_t> new_ico;
    new_ico.reserve(orig_data.size()); // max possible size

    // copy 6-byte header
    new_ico.insert(new_ico.end(), orig_data.data(), orig_data.data() + 6);

    // allocate space for new directory entries (16 bytes each)
    const size_t dir_start = new_ico.size();
    new_ico.resize(new_ico.size() + count * 16);

    uint32_t current_offset = static_cast<uint32_t>(new_ico.size());

    for (uint16_t i = 0; i < count; ++i) {
        const IcoEntryMeta& meta = (*metas)[i];
        std::vector<uint8_t> payload;

        if (meta.kind == IcoEntryMeta::Kind::Opaque) {
            payload = meta.raw_payload;
        } else if (meta.kind == IcoEntryMeta::Kind::Png) {
            payload = read_file(optimized_files[i]);
        } else {
            // BmpFixedUp: pull the (possibly re-encoded) color data back out of
            // the standalone BMP using its own header fields (bfOffBits, and the
            // DIB header's own width/height/bitcount, which are at fixed offsets
            // across all BITMAPINFOHEADER-family variants), then re-double the
            // height and re-attach the original AND mask/padding. Falls back to
            // the untouched original bytes if anything looks off, or if the
            // pipeline picked a compression method (e.g. bmplib's OS/2-dialect
            // RLE24, which reuses Windows' BI_JPEG code point) that standard
            // ICO/BMP readers wouldn't recognize inside an icon resource.
            bool fallback = true;
            const auto bmp = read_file(optimized_files[i]);
            if (bmp.size() >= 14 + 40 && bmp[0] == 'B' && bmp[1] == 'M') {
                const uint32_t bf_off_bits = read_le32(bmp.data() + 10);
                const uint32_t dib_size = read_le32(bmp.data() + 14);
                const uint32_t width = read_le32(bmp.data() + 14 + 4);
                const uint32_t true_height = read_le32(bmp.data() + 14 + 8);
                const uint16_t bitcount = read_le16(bmp.data() + 14 + 14);
                const uint32_t compression = dib_size >= 20 ? read_le32(bmp.data() + 14 + 16) : 0;
                const uint32_t row_size = bmp_row_size(width, bitcount);
                const uint64_t xor_data_size = static_cast<uint64_t>(row_size) * true_height;

                // only BI_RGB/BI_RLE8/BI_RLE4/BI_BITFIELDS are safe inside an ICO
                const bool compression_safe = compression <= 3;

                if (compression_safe && bf_off_bits + xor_data_size <= bmp.size()) {
                    std::vector<uint8_t> dib_header(bmp.data() + 14, bmp.data() + 14 + dib_size);
                    write_le32(dib_header.data() + 8, true_height * 2); // restore ICO's doubled-height convention
                    if (dib_size >= 24 && read_le32(dib_header.data() + 20) != 0) {
                        write_le32(dib_header.data() + 20, static_cast<uint32_t>(xor_data_size) + static_cast<uint32_t>(meta.trailing.size()));
                    }

                    payload.insert(payload.end(), dib_header.begin(), dib_header.end());
                    payload.insert(payload.end(), bmp.data() + 14 + dib_size, bmp.data() + bf_off_bits); // palette, if any
                    payload.insert(payload.end(), bmp.data() + bf_off_bits, bmp.data() + bf_off_bits + xor_data_size);
                    payload.insert(payload.end(), meta.trailing.begin(), meta.trailing.end());
                    fallback = false;
                } else {
                    Logger::log(LogLevel::Warning,
                        "ICO entry " + std::to_string(i) + " optimization used an incompatible BMP compression "
                        "method, keeping the original bytes", get_name());
                }
            }

            if (fallback) {
                payload = meta.raw_payload;
            }
        }

        // copy original 16-byte entry to preserve width/height/planes/bpp metadata
        const size_t orig_dir_offset = 6 + i * 16;
        uint8_t entry[16];
        memcpy(entry, orig_data.data() + orig_dir_offset, 16);

        // update size and offset
        write_le32(entry + 8, static_cast<uint32_t>(payload.size()));
        write_le32(entry + 12, current_offset);

        // write updated entry back into new_ico directory space
        memcpy(new_ico.data() + dir_start + i * 16, entry, 16);

        // append payload
        new_ico.insert(new_ico.end(), payload.begin(), payload.end());
        current_offset += static_cast<uint32_t>(payload.size());
    }

    std::filesystem::path output_path = std::filesystem::temp_directory_path() /
        (content.original_path.stem().string() + "_final" + RandomUtils::random_suffix() + content.original_path.extension().string());

    std::ofstream out_file(output_path, std::ios::binary);
    if (!out_file) throw std::runtime_error("CANNOT OPEN OUTPUT FILE: " + output_path.string());

    out_file.write(reinterpret_cast<const char*>(new_ico.data()), new_ico.size());
    out_file.close();

    if (out_file.fail()) {
        throw std::runtime_error("FAILED TO WRITE ICO OUTPUT DATA");
    }

    cleanup_temp_dir(content.temp_dir, get_name());
    Logger::log(LogLevel::Debug, "FINISHED ICO FINALIZATION", get_name());

    return output_path;
}

} // namespace chisel