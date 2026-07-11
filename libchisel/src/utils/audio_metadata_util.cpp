//
// Created by Giuseppe Francione on 17/11/25.
//

#include "audio_metadata_util.hpp"
#include <fstream>
#include <iterator>
#include <setjmp.h>
#include <vector>
#include <map>
#include <cstring>
#include <taglib/fileref.h>
#include "flac/flacfile.h"
#include "flac/flacpicture.h"
#include "mp4/mp4coverart.h"
#include "mp4/mp4file.h"
#include "mpeg/mpegfile.h"
#include "mpeg/id3v2/id3v2tag.h"
#include "mpeg/id3v2/frames/attachedpictureframe.h"
#include "ogg/xiphcomment.h"
#include "ogg/opus/opusfile.h"
#include "ogg/vorbis/vorbisfile.h"
#include "../../include/logger.hpp"
#include "../../include/file_utils.hpp"
#include "../../include/random_utils.hpp"
#include <png.h>
#include <jpeglib.h>
#include <webp/decode.h>
#include "aiff/aifffile.h"
#include "ape/apefile.h"
#include "ape/apeitem.h"
#include "ape/apetag.h"
#include "wav/wavfile.h"
#include "matroska/matroskafile.h"
#include "wavpack/wavpackfile.h"
#include "mpc/mpcfile.h"
#include "trueaudio/trueaudiofile.h"
#include "ogg/flac/oggflacfile.h"
#include "ogg/speex/speexfile.h"
#include "asf/asffile.h"
#include "asf/asfpicture.h"
#include "dsf/dsffile.h"
#include "dsdiff/dsdifffile.h"


namespace chisel {

//
// helpers
//

namespace {

// read file into bytevector
TagLib::ByteVector readFileToByteVector(const std::filesystem::path &p) {
    std::ifstream in(p, std::ios::binary);
    const std::vector<char> buffer((std::istreambuf_iterator<char>(in)), {});
    return TagLib::ByteVector(buffer.data(), static_cast<unsigned int>(buffer.size()));
}

// decide extension from mime
const char* extFromMime(const std::string &mime) {
    if (mime == "image/png") return ".png";
    if (mime == "image/jpeg" || mime == "image/jpg") return ".jpg";
    if (mime == "image/webp") return ".webp";
    if (mime == "image/gif") return ".gif";
    if (mime == "image/bmp") return ".bmp";
    if (mime == "image/tiff") return ".tiff";
    if (mime == "application/x-truetype-font" || mime == "font/ttf") return ".ttf";
    if (mime == "application/vnd.ms-opentype" || mime == "font/otf") return ".otf";
    if (mime == "font/woff2") return ".woff2";
    if (mime == "font/woff") return ".woff";
    if (mime == "text/xml" || mime == "application/xml") return ".xml";
    return ".bin";
}

// guess image mime type from magic bytes, for containers (APEv2) that don't
// store an explicit mime type alongside the cover art binary; falls back to
// "image/jpeg" since that's the overwhelmingly common case in the wild
std::string guessImageMimeFromMagic(const TagLib::ByteVector &data) {
    const auto *bytes = reinterpret_cast<const unsigned char *>(data.data());
    const auto size = data.size();

    if (size >= 8 && !png_sig_cmp(bytes, 0, 8)) return "image/png";
    if (size >= 6 && (std::memcmp(bytes, "GIF87a", 6) == 0 || std::memcmp(bytes, "GIF89a", 6) == 0)) return "image/gif";
    if (size >= 2 && bytes[0] == 'B' && bytes[1] == 'M') return "image/bmp";
    if (size >= 12 && std::memcmp(bytes, "RIFF", 4) == 0 && std::memcmp(bytes + 8, "WEBP", 4) == 0) return "image/webp";
    if (size >= 4 && (std::memcmp(bytes, "II*\0", 4) == 0 || std::memcmp(bytes, "MM\0*", 4) == 0)) return "image/tiff";
    return "image/jpeg";
}

// infer mp4 format from mime
TagLib::MP4::CoverArt::Format inferFormatFromMime(const std::string &mime) {
    return (mime == "image/png") ? TagLib::MP4::CoverArt::PNG : TagLib::MP4::CoverArt::JPEG;
}

// raii wrapper for file pointers
struct FileCloser {
    void operator()(FILE *f) const { if (f != nullptr) std::fclose(f); }
};
using unique_FILE = std::unique_ptr<FILE, FileCloser>;

// libpng error handlers (quiet)
void png_error_fn_quiet(const png_structp png, const png_const_charp msg) {
    Logger::log(LogLevel::Debug, std::string("Libpng (header read): ") + msg, "AudioMetadataUtil");
    longjmp(png_jmpbuf(png), 1);
}
void png_warning_fn_quiet(png_structp, const png_const_charp msg) {
    Logger::log(LogLevel::Debug, std::string("Libpng (header read warn): ") + msg, "AudioMetadataUtil");
}

// raii wrapper for libpng read structs
struct PngRead {
    png_structp png = nullptr;
    png_infop info = nullptr;
    explicit PngRead() {
        png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
        if (png != nullptr) {
            png_set_error_fn(png, nullptr, png_error_fn_quiet, png_warning_fn_quiet);
            info = png_create_info_struct(png);
        }
    }
    ~PngRead() {
        if ((png != nullptr) || (info != nullptr)) png_destroy_read_struct(&png, &info, nullptr);
    }
    [[nodiscard]] bool isValid() const { return (png != nullptr) && (info != nullptr); }
};

// libjpeg error handlers
struct JpegErrorMgr {
    jpeg_error_mgr pub{};
    jmp_buf setjmp_buffer{};
};
void jpeg_error_exit_throw(const j_common_ptr cinfo) {
    auto* err = reinterpret_cast<JpegErrorMgr*>(cinfo->err);
    // return control to computeImageProps
    longjmp(err->setjmp_buffer, 1);
}

// compute image props
void computeImageProps(const std::filesystem::path &imagePath,
                              const std::string& mime_type,
                              int &width, int &height, int &depth, int &colors) {
    // default to 0
    width = 0; height = 0; depth = 0; colors = 0;

    if (mime_type == "image/png") {
        try {
            unique_FILE fp(chisel::open_file(imagePath, "rb"));
            if (!fp) return;

            unsigned char sig[8];
            if (fread(sig, 1, 8, fp.get()) != 8 || png_sig_cmp(sig, 0, 8)) {
                return; // not a png
            }
            rewind(fp.get());

            PngRead rd;
            if (!rd.isValid() || (setjmp(png_jmpbuf(rd.png)) != 0)) {
                return; // libpng error
            }

            png_init_io(rd.png, fp.get());
            png_read_info(rd.png, rd.info);

            png_uint_32 w;
            png_uint_32 h;
            int bit_depth;
            int color_type;
            png_get_IHDR(rd.png, rd.info, &w, &h, &bit_depth, &color_type, nullptr, nullptr, nullptr);

            width = static_cast<int>(w);
            height = static_cast<int>(h);

            if (color_type == PNG_COLOR_TYPE_PALETTE) {
                depth = bit_depth; // 1, 2, 4, or 8
                png_colorp palette;
                int num_palette;
                if (png_get_PLTE(rd.png, rd.info, &palette, &num_palette) == PNG_INFO_PLTE) {
                    colors = num_palette;
                }
            } else {
                int channels = png_get_channels(rd.png, rd.info);
                depth = bit_depth * channels; // total bits per pixel
                colors = 0; // not indexed
            }
            return; // success
        } catch (const std::exception& e) {
            Logger::log(LogLevel::Warning, "Failed to read png header: " + imagePath.string() + " (" + e.what() + ")", "AudioMetadataUtil");
            return;
        }
    }

    if (mime_type == "image/jpeg" || mime_type == "image/jpg") {
        jpeg_decompress_struct cinfo{};
        JpegErrorMgr jsrcerr{};

        unique_FILE fp(chisel::open_file(imagePath, "rb"));
        if (!fp) return;

        cinfo.err = jpeg_std_error(&jsrcerr.pub);
        jsrcerr.pub.error_exit = jpeg_error_exit_throw;

        if (setjmp(jsrcerr.setjmp_buffer)) {
            // if libjpeg hits an error, it will jump here
            jpeg_destroy_decompress(&cinfo);
            Logger::log(LogLevel::Warning, "Failed to read jpeg header: " + imagePath.string(), "AudioMetadataUtil");
            return;
        }

        jpeg_create_decompress(&cinfo);
        jpeg_stdio_src(&cinfo, fp.get());
        jpeg_read_header(&cinfo, TRUE);

        // call abort to release file handle, we don't need to decompress
        jpeg_abort_decompress(&cinfo);

        width = static_cast<int>(cinfo.image_width);
        height = static_cast<int>(cinfo.image_height);
        depth = cinfo.num_components * cinfo.data_precision; // e.g., 8*3=24 for rgb
        colors = 0; // not indexed

        jpeg_destroy_decompress(&cinfo);
        return; // success
    }

    if (mime_type == "image/webp") {
        unique_FILE fp(chisel::open_file(imagePath, "rb"));
        if (!fp) return;

        // read basic header (30 bytes is enough for WebPGetInfo)
        uint8_t header[32];
        if (fread(header, 1, 30, fp.get()) < 30) return;

        if (WebPGetInfo(header, 30, &width, &height)) {
            depth = 32; // webp is generally RGBA 8888 internally or similar
            colors = 0;
        }
        return;
    }

    // TODO: implement for jxl, etc. if needed
    Logger::log(LogLevel::Debug, "Computeimageprops: unsupported mime type: " + mime_type, "AudioMetadataUtil");
}

// normalize picture type flac
int normalizePictureTypeFromFlac(const TagLib::FLAC::Picture::Type t) {
    return t;
}

// default front cover type (id3v2/flac standard for frontcover is 3)
int defaultFrontCoverType() {
    return 3;
}


// shared helper to extract cover from ID3v2 tag (MP3, WAV, AIFF)
void extractId3v2Covers(TagLib::ID3v2::Tag* tag,
                        const std::filesystem::path& temp_dir,
                        std::vector<AudioCoverInfo>& extracted_covers) {
    if (!tag) return;

    int idx = 0;
    auto frames = tag->frameList("APIC");
    for (auto frame : frames) {
        auto *apic = static_cast<TagLib::ID3v2::AttachedPictureFrame*>(frame);

        const char *ext = extFromMime(apic->mimeType().to8Bit(true));
        std::filesystem::path outPath = temp_dir / ("cover_" + std::to_string(idx) + ext);

        std::ofstream out(outPath, std::ios::binary);
        out.write(apic->picture().data(), apic->picture().size());
        out.close();

        AudioCoverInfo info;
        info.temp_file_path = outPath;
        info.mime_type = apic->mimeType().to8Bit(true);
        info.description = apic->description().to8Bit(true);
        info.picture_type = apic->type();

        extracted_covers.push_back(std::move(info));
        ++idx;
    }
}

// shared helper to reinsert cover in ID3v2 tag
// returns true if edits were made
bool rebuildId3v2Covers(TagLib::ID3v2::Tag* tag,
                        const std::vector<AudioCoverInfo>& covers) {
    if (!tag) return false;

    // remove all existing APIC frames to avoid duplicates
    tag->removeFrames("APIC");

    for (const auto &info : covers) {
        TagLib::ByteVector data = readFileToByteVector(info.temp_file_path);
        if (data.isEmpty()) continue;

        auto *frame = new TagLib::ID3v2::AttachedPictureFrame;
        frame->setMimeType(info.mime_type);
        frame->setDescription(info.description);
        frame->setType(static_cast<TagLib::ID3v2::AttachedPictureFrame::Type>(info.picture_type));
        frame->setPicture(data);

        tag->addFrame(frame);
    }
    return true;
}


int getPictureTypeFromApeKey(const TagLib::String &key) {
    const TagLib::String upperKey = key.upper();
    if (upperKey == "COVER ART (BACK)") return 4;
    if (upperKey == "COVER ART (LEAFLET)") return 5;
    if (upperKey == "COVER ART (MEDIA)") return 6;
    if (upperKey == "COVER ART (LEAD ARTIST)") return 8;
    if (upperKey == "COVER ART (ICON)") return 1;
    return 3; // default front
}

void extractApeV2Covers(TagLib::APE::Tag* tag,
                        const std::filesystem::path& temp_dir,
                        std::vector<AudioCoverInfo>& extracted_covers) {
    if (tag == nullptr) {
        return;
    }
    const auto &itemListMap = tag->itemListMap();
    int idx = static_cast<int>(extracted_covers.size());

    // iterate all items dynamically
    for (const auto & it : itemListMap) {
        TagLib::String key = it.first;

        // itemListMap keys are always upper-cased by TagLib::APE::Tag::parse() when read from
        // a file, regardless of the case originally written, so the comparison must match that.
        if (key.upper().startsWith("COVER ART ") && it.second.type() == TagLib::APE::Item::Binary) {
            TagLib::ByteVector val = it.second.binaryData();
            int nullPos = val.find(0);
            if (nullPos < 0) continue;

            TagLib::ByteVector imgData = val.mid(nullPos + 1);
            TagLib::String desc = TagLib::String(val.mid(0, nullPos), TagLib::String::UTF8);

            std::string mime = guessImageMimeFromMagic(imgData);
            const char *ext = extFromMime(mime);
            std::filesystem::path outPath = temp_dir / ("cover_" + std::to_string(idx) + ext);

            std::ofstream out(outPath, std::ios::binary);
            out.write(imgData.data(), imgData.size());
            out.close();

            AudioCoverInfo info;
            info.temp_file_path = outPath;
            info.mime_type = mime;
            info.description = desc.to8Bit(true);
            info.picture_type = getPictureTypeFromApeKey(key);

            // store original key to rebuild properly
            info.format_specific = std::make_any<std::string>(key.to8Bit(true));

            extracted_covers.push_back(std::move(info));
            ++idx;
        }
    }
}

bool rebuildApeV2Covers(TagLib::APE::Tag* tag,
                        const std::vector<AudioCoverInfo>& covers) {
    if (tag == nullptr) return false;

    // clean up all existing covers dynamically (map keys are always upper-cased by TagLib)
    auto itemList = tag->itemListMap();
    for (auto & it : itemList) {
        if (it.first.upper().startsWith("COVER ART ")) {
            tag->removeItem(it.first);
        }
    }

    for (const auto &info : covers) {
        TagLib::ByteVector data = readFileToByteVector(info.temp_file_path);
        if (data.isEmpty()) continue;

        std::string key_str = "Cover Art (Front)"; // fallback
        if (info.format_specific.has_value() && info.format_specific.type() == typeid(std::string)) {
            key_str = std::any_cast<std::string>(info.format_specific);
        }
        TagLib::String key(key_str, TagLib::String::UTF8);

        TagLib::ByteVector val;
        val.append(TagLib::ByteVector(info.description.data(), static_cast<unsigned int>(info.description.size())));
        val.append(0);
        val.append(data);

        tag->setItem(key, TagLib::APE::Item(key, val, true));
    }
    return true;
}

// shared helper to extract covers from xiph comment (ogg variants)
void extractXiphCovers(TagLib::Ogg::XiphComment* tag,
                       const std::filesystem::path& temp_dir,
                       std::vector<AudioCoverInfo>& extracted_covers) {
    if (tag == nullptr) return;

    int idx = static_cast<int>(extracted_covers.size());
    for (auto *pic : tag->pictureList()) {
        const char *ext = extFromMime(pic->mimeType().to8Bit(true));
        std::filesystem::path outPath = temp_dir / ("cover_" + std::to_string(idx) + ext);

        std::ofstream out(outPath, std::ios::binary);
        out.write(pic->data().data(), pic->data().size());
        out.close();

        AudioCoverInfo info;
        info.temp_file_path = outPath;
        info.mime_type = pic->mimeType().to8Bit(true);
        info.description = pic->description().to8Bit(true);
        info.picture_type = normalizePictureTypeFromFlac(pic->type());

        extracted_covers.push_back(std::move(info));
        ++idx;
    }
}

// shared helper to rebuild xiph comment covers
bool rebuildXiphCovers(TagLib::Ogg::XiphComment* tag,
                       const std::vector<AudioCoverInfo>& covers) {
    if (tag == nullptr) return false;

    tag->removeAllPictures();

    for (const auto &info : covers) {
        TagLib::ByteVector data = readFileToByteVector(info.temp_file_path);
        if (data.isEmpty()) continue;

        auto *pic = new TagLib::FLAC::Picture;
        pic->setMimeType(info.mime_type);
        pic->setDescription(info.description);
        pic->setType(static_cast<TagLib::FLAC::Picture::Type>(info.picture_type));
        pic->setData(data);

        int w=0;
        int h=0;
        int d=0;
        int c=0;
        computeImageProps(info.temp_file_path, info.mime_type, w, h, d, c);
        if (w > 0) pic->setWidth(w);
        if (h > 0) pic->setHeight(h);
        if (d > 0) pic->setColorDepth(d);
        if (c > 0) pic->setNumColors(c);

        tag->addPicture(pic);
    }
    return true;
}

} // namespace

//
// extraction
//

AudioExtractionState AudioMetadataUtil::extractCovers(const std::filesystem::path &input_path,
                                                      const std::filesystem::path &temp_dir) {
    AudioExtractionState state;

#ifdef _WIN32
    TagLib::FileRef ref(input_path.wstring().c_str());
#else
    TagLib::FileRef ref(input_path.string().c_str());
#endif
    TagLib::File *file_ref = ref.file();
    if (file_ref == nullptr) {
        return state;
    }

    // flac
    if (auto *flacFile = dynamic_cast<TagLib::FLAC::File*>(file_ref)) {
        int idx = 0;
        for (auto *pic : flacFile->pictureList()) {
            const char *ext = extFromMime(pic->mimeType().to8Bit(true));
            std::filesystem::path outPath = temp_dir / ("cover_" + std::to_string(idx) + ext);

            std::ofstream out(outPath, std::ios::binary);
            out.write(pic->data().data(), pic->data().size());
            out.close();

            AudioCoverInfo info;
            info.temp_file_path = outPath;
            info.mime_type = pic->mimeType().to8Bit(true);
            info.description = pic->description().to8Bit(true);
            info.picture_type = normalizePictureTypeFromFlac(pic->type());
            // note: flac picture fields (w, h, etc.) will be computed on reinsertion

            state.extracted_covers.push_back(std::move(info));
            ++idx;
        }
        return state;
    }

    // mp3 (id3v2 apic)
    if (auto *mpegFile = dynamic_cast<TagLib::MPEG::File*>(file_ref)) {
        extractId3v2Covers(mpegFile->ID3v2Tag(), temp_dir, state.extracted_covers);
        return state;
    }

    // wav (id3v2 apic)
    if (auto *wavFile = dynamic_cast<TagLib::RIFF::WAV::File*>(file_ref)) {
        extractId3v2Covers(wavFile->ID3v2Tag(), temp_dir, state.extracted_covers);
        return state;
    }

    // aiff (id3v2 apic)
    if (auto *aiffFile = dynamic_cast<TagLib::RIFF::AIFF::File*>(file_ref)) {
        extractId3v2Covers(aiffFile->tag(), temp_dir, state.extracted_covers);
        return state;
    }

    // ape (Monkey's Audio) - use APEv2
    if (auto *apeFile = dynamic_cast<TagLib::APE::File*>(file_ref)) {
        extractApeV2Covers(apeFile->APETag(), temp_dir, state.extracted_covers);
        return state;
    }

    // mp4
    if (auto *mp4File = dynamic_cast<TagLib::MP4::File*>(file_ref)) {
        auto *tag = mp4File->tag();
        if (tag != nullptr) {
            auto items = tag->itemMap();
            auto it = items.find("covr");
            if (it != items.end()) {
                int idx = 0;
                const TagLib::MP4::CoverArtList &covers = it->second.toCoverArtList();

                for (const auto &cover : covers) {
                    const char *ext = (cover.format() == TagLib::MP4::CoverArt::PNG) ? ".png" : ".jpg";
                    std::filesystem::path outPath = temp_dir / ("cover_" + std::to_string(idx) + ext);

                    std::ofstream out(outPath, std::ios::binary);
                    out.write(cover.data().data(), cover.data().size());
                    out.close();

                    AudioCoverInfo info;
                    info.temp_file_path = outPath;
                    info.mime_type = (cover.format() == TagLib::MP4::CoverArt::PNG) ? "image/png" : "image/jpeg";
                    info.description = ""; // mp4 doesn't store description/type per cover
                    info.picture_type = defaultFrontCoverType();
                    info.format_specific = cover.format(); // save format for reinsertion

                    state.extracted_covers.push_back(std::move(info));
                    ++idx;
                }
            }
        }
        return state;
    }

    // ogg vorbis
    if (auto *oggVorbis = dynamic_cast<TagLib::Ogg::Vorbis::File*>(file_ref)) {
        auto *xc = oggVorbis->tag();
        if (xc != nullptr) {
            int idx = 0;
            auto pics = xc->pictureList();
            for (auto *pic : pics) {
                const char *ext = extFromMime(pic->mimeType().to8Bit(true));
                std::filesystem::path outPath = temp_dir / ("cover_" + std::to_string(idx) + ext);

                std::ofstream out(outPath, std::ios::binary);
                out.write(pic->data().data(), pic->data().size());
                out.close();

                AudioCoverInfo info;
                info.temp_file_path = outPath;
                info.mime_type = pic->mimeType().to8Bit(true);
                info.description = pic->description().to8Bit(true);
                info.picture_type = normalizePictureTypeFromFlac(pic->type());

                state.extracted_covers.push_back(std::move(info));
                ++idx;
            }
        }
        return state;
    }

    // ogg opus
    if (auto *oggOpus = dynamic_cast<TagLib::Ogg::Opus::File*>(file_ref)) {
        auto *xc = oggOpus->tag();
        if (xc != nullptr) {
            int idx = 0;
            auto pics = xc->pictureList();
            for (auto *pic : pics) {
                const char *ext = extFromMime(pic->mimeType().to8Bit(true));
                std::filesystem::path outPath = temp_dir / ("cover_" + std::to_string(idx) + ext);

                std::ofstream out(outPath, std::ios::binary);
                out.write(pic->data().data(), pic->data().size());
                out.close();

                AudioCoverInfo info;
                info.temp_file_path = outPath;
                info.mime_type = pic->mimeType().to8Bit(true);
                info.description = pic->description().to8Bit(true);
                info.picture_type = normalizePictureTypeFromFlac(pic->type());

                state.extracted_covers.push_back(std::move(info));
                ++idx;
            }
        }
        return state;
    }

    // mkv (matroska / webm)
    if (auto *mkvFile = dynamic_cast<TagLib::Matroska::File*>(file_ref)) {
        int idx = 0;

        // TagLib::Matroska::File::complexProperties("ATTACHMENT") is *not* a generic
        // "all non-picture attachments" query: it only matches attachments whose
        // filename/mimeType/uid is literally the string "ATTACHMENT", which real
        // files never have. complexPropertyKeys() is the correct way to enumerate
        // every attachment (it yields "PICTURE" for images, and each attachment's
        // own filename otherwise), so fonts/xml/etc. are picked up too.
        for (const auto &key : mkvFile->complexPropertyKeys()) {
            for (const auto &attMap : mkvFile->complexProperties(key)) {
                if (!attMap.contains("data") || !attMap.contains("mimeType")) continue;

                TagLib::ByteVector data = attMap["data"].toByteVector();
                std::string mime = attMap["mimeType"].toString().to8Bit(true);
                std::string original_fileName = attMap.contains("fileName") ? attMap["fileName"].toString().to8Bit(true) : "";

                // prefer the attachment's own filename extension (e.g. font.woff2)
                // so it gets picked up by the matching processor; fall back to a
                // mime-based guess only if the filename has none
                std::string ext = std::filesystem::path(original_fileName).extension().string();
                if (ext.empty()) ext = extFromMime(mime);
                std::filesystem::path outPath = temp_dir / ("attachment_" + std::to_string(idx) + ext);

                std::ofstream out(outPath, std::ios::binary);
                out.write(data.data(), data.size());
                out.close();

                AudioCoverInfo info;
                info.temp_file_path = outPath;
                info.mime_type = mime;
                info.picture_type = defaultFrontCoverType();

                if (attMap.contains("description")) {
                    info.description = attMap["description"].toString().to8Bit(true);
                }

                // store the original TagLib complex-property key and filename so
                // finalize can group entries back under the same key on rebuild
                // (multiple pictures all share the "PICTURE" key; every other
                // attachment is keyed by its own filename)
                info.format_specific = std::make_any<std::pair<std::string, std::string>>(key.to8Bit(true), original_fileName);

                state.extracted_covers.push_back(std::move(info));
                ++idx;
            }
        }
        return state;
    }

    // trueaudio (id3v2 apic)
    if (auto *ttaFile = dynamic_cast<TagLib::TrueAudio::File*>(file_ref)) {
        extractId3v2Covers(ttaFile->ID3v2Tag(), temp_dir, state.extracted_covers);
        return state;
    }

    // wavpack (apev2)
    if (auto *wvFile = dynamic_cast<TagLib::WavPack::File*>(file_ref)) {
        extractApeV2Covers(wvFile->APETag(), temp_dir, state.extracted_covers);
        return state;
    }

    // musepack (apev2)
    if (auto *mpcFile = dynamic_cast<TagLib::MPC::File*>(file_ref)) {
        extractApeV2Covers(mpcFile->APETag(), temp_dir, state.extracted_covers);
        return state;
    }

    // ogg flac (xiph)
    if (auto *oggFlac = dynamic_cast<TagLib::Ogg::FLAC::File*>(file_ref)) {
        extractXiphCovers(oggFlac->tag(), temp_dir, state.extracted_covers);
        return state;
    }

    // ogg speex (xiph)
    if (auto *oggSpeex = dynamic_cast<TagLib::Ogg::Speex::File*>(file_ref)) {
        extractXiphCovers(oggSpeex->tag(), temp_dir, state.extracted_covers);
        return state;
    }

    // asf / wma / wmv
    if (auto *asfFile = dynamic_cast<TagLib::ASF::File*>(file_ref)) {
        if (auto *tag = asfFile->tag()) {
            auto attrList = tag->attributeListMap()["WM/Picture"];
            int idx = 0;
            for (const auto &attr : attrList) {
                TagLib::ASF::Picture pic = attr.toPicture();
                if (!pic.isValid()) continue;

                std::string mime = pic.mimeType().to8Bit(true);
                const char *ext = extFromMime(mime);
                std::filesystem::path outPath = temp_dir / ("cover_" + std::to_string(idx) + ext);

                std::ofstream out(outPath, std::ios::binary);
                out.write(pic.picture().data(), pic.picture().size());
                out.close();

                AudioCoverInfo info;
                info.temp_file_path = outPath;
                info.mime_type = mime;
                info.description = pic.description().to8Bit(true);
                info.picture_type = pic.type();

                state.extracted_covers.push_back(std::move(info));
                ++idx;
            }
        }
        return state;
    }

    // dsf
    if (auto *dsfFile = dynamic_cast<TagLib::DSF::File*>(file_ref)) {
        extractId3v2Covers(dsfFile->tag(), temp_dir, state.extracted_covers);
        return state;
    }

    // dsdiff (id3v2 apic)
    if (auto *dsdiffFile = dynamic_cast<TagLib::DSDIFF::File*>(file_ref)) {
        extractId3v2Covers(dsdiffFile->ID3v2Tag(), temp_dir, state.extracted_covers);
        return state;
    }

    return state;
}

//
// reinsertion
//

bool AudioMetadataUtil::rebuildCovers(const std::filesystem::path &input_path,
                                      const AudioExtractionState &state) {
#ifdef _WIN32
    TagLib::FileRef ref(input_path.wstring().c_str());
#else
    TagLib::FileRef ref(input_path.string().c_str());
#endif
    TagLib::File *file_ref = ref.file();
    if (file_ref == nullptr) {
        return false;
    }

    // flac
    if (auto *flacFile = dynamic_cast<TagLib::FLAC::File*>(file_ref)) {
        flacFile->removePictures();

        for (const auto &info : state.extracted_covers) {
            TagLib::ByteVector data = readFileToByteVector(info.temp_file_path);
            if (data.isEmpty()) continue;

            auto *pic = new TagLib::FLAC::Picture;
            pic->setMimeType(info.mime_type);
            pic->setDescription(info.description);
            pic->setType(static_cast<TagLib::FLAC::Picture::Type>(info.picture_type));
            pic->setData(data);

            // compute technical fields from the *optimized* image data
            int w=0, h=0, d=0, c=0;
            computeImageProps(info.temp_file_path, info.mime_type, w, h, d, c);
            if (w > 0) pic->setWidth(w);
            if (h > 0) pic->setHeight(h);
            if (d > 0) pic->setColorDepth(d);
            if (c > 0) pic->setNumColors(c);

            flacFile->addPicture(pic);
        }
        return flacFile->save();
    }

    // mp3
    if (auto *mpegFile = dynamic_cast<TagLib::MPEG::File*>(file_ref)) {
        if (rebuildId3v2Covers(mpegFile->ID3v2Tag(true), state.extracted_covers)) {
            return mpegFile->save();
        }
        return false;
    }

    // wav
    if (auto *wavFile = dynamic_cast<TagLib::RIFF::WAV::File*>(file_ref)) {
        if (rebuildId3v2Covers(wavFile->ID3v2Tag(), state.extracted_covers)) {
            return wavFile->save();
        }
        return false;
    }

    // aiff
    if (auto *aiffFile = dynamic_cast<TagLib::RIFF::AIFF::File*>(file_ref)) {
        if (rebuildId3v2Covers(aiffFile->tag(), state.extracted_covers)) {
            return aiffFile->save();
        }
        return false;
    }

    // ape
    if (auto *apeFile = dynamic_cast<TagLib::APE::File*>(file_ref)) {
        if (rebuildApeV2Covers(apeFile->APETag(true), state.extracted_covers)) {
            return apeFile->save();
        }
        return false;
    }

    // mp4
    if (auto *mp4File = dynamic_cast<TagLib::MP4::File*>(file_ref)) {
        TagLib::MP4::CoverArtList covers;

        for (const auto &info : state.extracted_covers) {
            TagLib::ByteVector data = readFileToByteVector(info.temp_file_path);
            if (data.isEmpty()) continue;

            TagLib::MP4::CoverArt::Format fmt;
            if (info.format_specific.has_value() &&
                info.format_specific.type() == typeid(TagLib::MP4::CoverArt::Format)) {
                fmt = std::any_cast<TagLib::MP4::CoverArt::Format>(info.format_specific);
            } else {
                fmt = inferFormatFromMime(info.mime_type);
            }

            covers.append(TagLib::MP4::CoverArt(fmt, data));
        }

        auto *tag = mp4File->tag();
        if (tag == nullptr) return false;

        tag->removeItem("covr");
        tag->setItem("covr", TagLib::MP4::Item(covers)); // create item from list

        return mp4File->save();
    }

    // ogg vorbis
    if (auto *oggVorbis = dynamic_cast<TagLib::Ogg::Vorbis::File*>(file_ref)) {
        auto *xc = oggVorbis->tag();
        if (xc == nullptr) return false;

        xc->removeAllPictures();

        for (const auto &info : state.extracted_covers) {
            TagLib::ByteVector data = readFileToByteVector(info.temp_file_path);
            if (data.isEmpty()) continue;

            auto *pic = new TagLib::FLAC::Picture;
            pic->setMimeType(info.mime_type);
            pic->setDescription(info.description);
            pic->setType(static_cast<TagLib::FLAC::Picture::Type>(info.picture_type));
            pic->setData(data);

            int w=0;
            int h=0;
            int d=0;
            int c=0;
            computeImageProps(info.temp_file_path, info.mime_type, w, h, d, c);
            if (w > 0) pic->setWidth(w);
            if (h > 0) pic->setHeight(h);
            if (d > 0) pic->setColorDepth(d);
            if (c > 0) pic->setNumColors(c);

            xc->addPicture(pic);
        }
        return oggVorbis->save();
    }

    // ogg opus
    if (auto *oggOpus = dynamic_cast<TagLib::Ogg::Opus::File*>(file_ref)) {
        auto *xc = oggOpus->tag();
        if (xc == nullptr) return false;

        xc->removeAllPictures();

        for (const auto &info : state.extracted_covers) {
            TagLib::ByteVector data = readFileToByteVector(info.temp_file_path);
            if (data.isEmpty()) continue;

            auto *pic = new TagLib::FLAC::Picture;
            pic->setMimeType(info.mime_type);
            pic->setDescription(info.description);
            pic->setType(static_cast<TagLib::FLAC::Picture::Type>(info.picture_type));
            pic->setData(data);

            int w=0, h=0, d=0, c=0;
            computeImageProps(info.temp_file_path, info.mime_type, w, h, d, c);
            if (w > 0) pic->setWidth(w);
            if (h > 0) pic->setHeight(h);
            if (d > 0) pic->setColorDepth(d);
            if (c > 0) pic->setNumColors(c);

            xc->addPicture(pic);
        }
        return oggOpus->save();
    }

    // mkv (matroska / webm)
    if (auto *mkvFile = dynamic_cast<TagLib::Matroska::File*>(file_ref)) {
        // group by original TagLib complex-property key: setComplexProperties(key, list)
        // replaces *all* attachments under that key in one call, so entries sharing a
        // key (e.g. every picture shares "PICTURE") must be batched into a single call
        // rather than one call per attachment, or each call would wipe the previous one
        std::map<std::string, TagLib::List<TagLib::VariantMap>> grouped;

        for (const auto &info : state.extracted_covers) {
            TagLib::ByteVector data = readFileToByteVector(info.temp_file_path);
            if (data.isEmpty()) continue;

            std::string key = "PICTURE";
            std::string fileName = "attachment" + std::string(extFromMime(info.mime_type));

            if (info.format_specific.has_value() && info.format_specific.type() == typeid(std::pair<std::string, std::string>)) {
                auto meta = std::any_cast<std::pair<std::string, std::string>>(info.format_specific);
                if (!meta.first.empty()) key = meta.first;
                if (!meta.second.empty()) fileName = meta.second;
            }

            TagLib::VariantMap attMap;
            attMap.insert("data", data);
            attMap.insert("mimeType", TagLib::String(info.mime_type, TagLib::String::UTF8));
            attMap.insert("description", TagLib::String(info.description, TagLib::String::UTF8));
            attMap.insert("fileName", TagLib::String(fileName, TagLib::String::UTF8));

            grouped[key].append(attMap);
        }

        for (const auto &[key, list] : grouped) {
            mkvFile->setComplexProperties(TagLib::String(key, TagLib::String::UTF8), list);
        }
        return mkvFile->save();
    }

    // trueaudio
    if (auto *ttaFile = dynamic_cast<TagLib::TrueAudio::File*>(file_ref)) {
        if (rebuildId3v2Covers(ttaFile->ID3v2Tag(true), state.extracted_covers)) {
            return ttaFile->save();
        }
        return false;
    }

    // wavpack
    if (auto *wvFile = dynamic_cast<TagLib::WavPack::File*>(file_ref)) {
        if (rebuildApeV2Covers(wvFile->APETag(true), state.extracted_covers)) {
            return wvFile->save();
        }
        return false;
    }

    // musepack
    if (auto *mpcFile = dynamic_cast<TagLib::MPC::File*>(file_ref)) {
        if (rebuildApeV2Covers(mpcFile->APETag(true), state.extracted_covers)) {
            return mpcFile->save();
        }
        return false;
    }

    // ogg flac
    if (auto *oggFlac = dynamic_cast<TagLib::Ogg::FLAC::File*>(file_ref)) {
        if (rebuildXiphCovers(oggFlac->tag(), state.extracted_covers)) {
            return oggFlac->save();
        }
        return false;
    }

    // ogg speex
    if (auto *oggSpeex = dynamic_cast<TagLib::Ogg::Speex::File*>(file_ref)) {
        if (rebuildXiphCovers(oggSpeex->tag(), state.extracted_covers)) {
            return oggSpeex->save();
        }
        return false;
    }

    // asf / wma / wmv
    if (auto *asfFile = dynamic_cast<TagLib::ASF::File*>(file_ref)) {
        auto *tag = asfFile->tag();
        if (tag == nullptr) return false;

        tag->removeItem("WM/Picture");

        for (const auto &info : state.extracted_covers) {
            TagLib::ByteVector data = readFileToByteVector(info.temp_file_path);
            if (data.isEmpty()) continue;

            TagLib::ASF::Picture pic;
            pic.setMimeType(info.mime_type);
            pic.setDescription(info.description);
            pic.setType(static_cast<TagLib::ASF::Picture::Type>(info.picture_type));
            pic.setPicture(data);

            tag->addAttribute("WM/Picture", TagLib::ASF::Attribute(pic.render()));
        }
        return asfFile->save();
    }

    // dsf
    if (auto *dsfFile = dynamic_cast<TagLib::DSF::File*>(file_ref)) {
        if (rebuildId3v2Covers(dsfFile->tag(), state.extracted_covers)) {
            return dsfFile->save();
        }
        return false;
    }

    // dsdiff
    if (auto *dsdiffFile = dynamic_cast<TagLib::DSDIFF::File*>(file_ref)) {
        if (rebuildId3v2Covers(dsdiffFile->ID3v2Tag(true), state.extracted_covers)) {
            return dsdiffFile->save();
        }
        return false;
    }

    return false;
}

std::optional<ExtractedContent> AudioMetadataUtil::prepareCoverExtraction(
    const std::filesystem::path& input_path,
    const std::string& temp_dir_prefix,
    const std::string_view tag) {
    Logger::log(LogLevel::Debug, "Entering prepare_extraction for " + input_path.string(), tag);

    ExtractedContent content;
    content.original_path = input_path;
    content.temp_dir = make_temp_dir_for(input_path, temp_dir_prefix);

    AudioExtractionState state = extractCovers(input_path, content.temp_dir);

    if (state.extracted_covers.empty()) {
        Logger::log(LogLevel::Debug, "No embedded cover art found", tag);
        cleanup_temp_dir(content.temp_dir, tag);
        return std::nullopt;
    }

    for (const auto& cover_info : state.extracted_covers) {
        content.extracted_files.push_back(cover_info.temp_file_path);
    }

    content.extras = std::make_any<AudioExtractionState>(std::move(state));
    content.format = ContainerFormat::Unknown;

    Logger::log(LogLevel::Debug, "Exiting prepare_extraction for " + input_path.string(), tag);
    return content;
}

std::filesystem::path AudioMetadataUtil::finalizeCoverExtraction(
    const ExtractedContent& content,
    const std::string_view tag) {
    Logger::log(LogLevel::Debug, "Entering finalize_extraction for " + content.original_path.string(), tag);

    const auto* state_ptr = std::any_cast<AudioExtractionState>(&content.extras);
    if (state_ptr == nullptr) {
        Logger::log(LogLevel::Error, "Failed to retrieve extraction state", tag);
        cleanup_temp_dir(content.temp_dir, tag);
        return {};
    }

    const std::filesystem::path final_temp_path = std::filesystem::temp_directory_path() /
        (content.original_path.stem().string() + "_final" + RandomUtils::random_suffix() +
         content.original_path.extension().string());

    try {
        std::filesystem::copy_file(content.original_path, final_temp_path,
                                   std::filesystem::copy_options::overwrite_existing);
    } catch (const std::exception& e) {
        Logger::log(LogLevel::Error, "Failed to copy audio file: " + std::string(e.what()), tag);
        cleanup_temp_dir(content.temp_dir, tag);
        return {};
    }

    if (!rebuildCovers(final_temp_path, *state_ptr)) {
        Logger::log(LogLevel::Error, "RebuildCovers failed", tag);
        cleanup_temp_dir(content.temp_dir, tag);
        std::filesystem::remove(final_temp_path);
        return {};
    }

    cleanup_temp_dir(content.temp_dir, tag);
    Logger::log(LogLevel::Debug, "Exiting finalize_extraction for " + final_temp_path.string(), tag);
    return final_temp_path;
}

void AudioMetadataUtil::placeholderCopyRecompress(
    const std::filesystem::path& input,
    const std::filesystem::path& output,
    const std::string_view tag) {
    Logger::log(LogLevel::Debug, "Entering recompress for " + input.string(), tag);
    Logger::log(LogLevel::Warning, "Recompress called with a copy-only placeholder", tag);

    std::error_code ec;
    std::filesystem::copy_file(input, output, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        throw std::runtime_error("Placeholder recompress failed to copy file.");
    }

    Logger::log(LogLevel::Debug, "Exiting recompress for " + output.string(), tag);
}

} // namespace chisel