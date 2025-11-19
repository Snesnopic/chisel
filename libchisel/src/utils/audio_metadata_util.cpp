//
// Created by Giuseppe Francione on 17/11/25.
//

#include "audio_metadata_util.hpp"
#include <fstream>
#include <iterator>
#include <setjmp.h>
#include <vector>
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
#include <png.h>
#include <jpeglib.h>
#include <webp/decode.h>
#include "aiff/aifffile.h"
#include "ape/apefile.h"
#include "ape/apeitem.h"
#include "ape/apetag.h"
#include "wav/wavfile.h"

namespace chisel {

//
// helpers
//

namespace {

// read file into bytevector
inline TagLib::ByteVector readFileToByteVector(const std::filesystem::path &p) {
    std::ifstream in(p, std::ios::binary);
    const std::vector<char> buffer((std::istreambuf_iterator<char>(in)), {});
    return TagLib::ByteVector(buffer.data(), static_cast<unsigned int>(buffer.size()));
}

// decide extension from mime
inline const char* extFromMime(const std::string &mime) {
    if (mime == "image/png") return ".png";
    if (mime == "image/jpeg" || mime == "image/jpg") return ".jpg";
    if (mime == "image/webp") return ".webp";
    // fallback: prefer jpg for unknowns
    return ".jpg";
}

// infer mp4 format from mime
inline TagLib::MP4::CoverArt::Format inferFormatFromMime(const std::string &mime) {
    return (mime == "image/png") ? TagLib::MP4::CoverArt::PNG : TagLib::MP4::CoverArt::JPEG;
}

// raii wrapper for file pointers
struct FileCloser {
    void operator()(FILE *f) const { if (f) std::fclose(f); }
};
using unique_FILE = std::unique_ptr<FILE, FileCloser>;

// libpng error handlers (quiet)
void png_error_fn_quiet(png_structp, png_const_charp msg) {
    Logger::log(LogLevel::Debug, std::string("libpng (header read): ") + msg, "audio_util");
    throw std::runtime_error(msg);
}
void png_warning_fn_quiet(png_structp, png_const_charp msg) {
    Logger::log(LogLevel::Debug, std::string("libpng (header read warn): ") + msg, "audio_util");
}

// raii wrapper for libpng read structs
struct PngRead {
    png_structp png = nullptr;
    png_infop info = nullptr;
    explicit PngRead() {
        png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
        if (png) {
            png_set_error_fn(png, nullptr, png_error_fn_quiet, png_warning_fn_quiet);
            info = png_create_info_struct(png);
        }
    }
    ~PngRead() {
        if (png || info) png_destroy_read_struct(&png, &info, nullptr);
    }
    bool isValid() const { return png && info; }
};

// libjpeg error handlers
struct JpegErrorMgr {
    jpeg_error_mgr pub{};
    jmp_buf setjmp_buffer{};
};
void jpeg_error_exit_throw(j_common_ptr cinfo) {
    auto* err = reinterpret_cast<JpegErrorMgr*>(cinfo->err);
    // return control to computeImageProps
    longjmp(err->setjmp_buffer, 1);
}

// compute image props
inline void computeImageProps(const std::filesystem::path &imagePath,
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
            if (!rd.isValid() || setjmp(png_jmpbuf(rd.png))) {
                return; // libpng error
            }

            png_init_io(rd.png, fp.get());
            png_read_info(rd.png, rd.info);

            png_uint_32 w, h;
            int bit_depth, color_type;
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
            Logger::log(LogLevel::Warning, "failed to read png header: " + imagePath.string() + " (" + e.what() + ")", "audio_util");
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
            Logger::log(LogLevel::Warning, "failed to read jpeg header: " + imagePath.string(), "audio_util");
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
    Logger::log(LogLevel::Debug, "computeImageProps: unsupported mime type: " + mime_type, "audio_util");
}

// normalize picture type flac
inline int normalizePictureTypeFromFlac(const TagLib::FLAC::Picture::Type t) {
    return static_cast<int>(t);
}

// default front cover type (id3v2/flac standard for frontcover is 3)
inline int defaultFrontCoverType() {
    return 3;
}

// helper to map ID3/FLAC image type to APEv2 keys
TagLib::String getApeCoverKey(int picture_type) {
    // 3 = Front Cover, 4 = Back Cover. Default a Front.
    if (picture_type == 4) return "Cover Art (Back)";
    return "Cover Art (Front)";
}

// inverse helper
int getPictureTypeFromApeKey(const TagLib::String &key) {
    if (key == "Cover Art (Back)") return 4;
    return 3; // Default Front
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
    if (!file_ref) {
        return state;
    }

    // flac
    if (auto *flacFile = dynamic_cast<TagLib::FLAC::File*>(file_ref)) {
        int idx = 0;
        for (auto pic : flacFile->pictureList()) {
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
        if (auto *tag = apeFile->APETag()) {
            const auto &itemListMap = tag->itemListMap();

            const constexpr char* keys[] = {"Cover Art (Front)", "Cover Art (Back)"};

            int idx = 0;
            for (const auto* key : keys) {
                if (itemListMap.contains(key)) {
                    const auto &item = itemListMap[key];
                    if (item.type() != TagLib::APE::Item::Binary) continue;

                    TagLib::ByteVector val = item.binaryData();

                    int nullPos = val.find(0);
                    if (nullPos < 0) continue; // not valid

                    TagLib::ByteVector imgData = val.mid(nullPos + 1);
                    TagLib::String desc = TagLib::String(val.mid(0, nullPos), TagLib::String::UTF8);

                    std::string mime = "image/jpeg"; // fallback
                    if (imgData.size() >= 8 && !png_sig_cmp((unsigned char*)imgData.data(), 0, 8)) {
                        mime = "image/png";
                    }

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

                    state.extracted_covers.push_back(std::move(info));
                    ++idx;
                }
            }
        }
        return state;
    }

    // mp4
    if (auto *mp4File = dynamic_cast<TagLib::MP4::File*>(file_ref)) {
        auto *tag = mp4File->tag();
        if (tag) {
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
        if (xc) {
            int idx = 0;
            auto pics = xc->pictureList();
            for (auto pic : pics) {
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
        if (xc) {
            int idx = 0;
            auto pics = xc->pictureList();
            for (auto pic : pics) {
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
    if (!file_ref) {
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
        auto *tag = apeFile->APETag(true);
        if (!tag) return false;

        tag->removeItem("Cover Art (Front)");
        tag->removeItem("Cover Art (Back)");

        for (const auto &info : state.extracted_covers) {
            TagLib::ByteVector data = readFileToByteVector(info.temp_file_path);
            if (data.isEmpty()) continue;

            TagLib::String key = getApeCoverKey(info.picture_type);

            TagLib::ByteVector val;
            val.append(TagLib::ByteVector(info.description.data(), static_cast<unsigned int>(info.description.size())));
            val.append(0); // null terminator
            val.append(data);

            tag->setItem(key, TagLib::APE::Item(key, val, true));
        }
        return apeFile->save();
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
        if (!tag) return false;

        tag->removeItem("covr");
        tag->setItem("covr", TagLib::MP4::Item(covers)); // create item from list

        return mp4File->save();
    }

    // ogg vorbis
    if (auto *oggVorbis = dynamic_cast<TagLib::Ogg::Vorbis::File*>(file_ref)) {
        auto *xc = oggVorbis->tag();
        if (!xc) return false;

        xc->removeAllPictures();

        for (const auto &info : state.extracted_covers) {
            TagLib::ByteVector data = readFileToByteVector(info.temp_file_path);
            if (data.isEmpty()) continue;

            // ogg uses the same picture format as flac but doesn't own pointers
            TagLib::FLAC::Picture pic;
            pic.setMimeType(info.mime_type);
            pic.setDescription(info.description);
            pic.setType(static_cast<TagLib::FLAC::Picture::Type>(info.picture_type));
            pic.setData(data);

            int w=0, h=0, d=0, c=0;
            computeImageProps(info.temp_file_path, info.mime_type, w, h, d, c);
            if (w > 0) pic.setWidth(w);
            if (h > 0) pic.setHeight(h);
            if (d > 0) pic.setColorDepth(d);
            if (c > 0) pic.setNumColors(c);

            xc->addPicture(&pic); // taglib base64-encodes this automatically
        }
        return oggVorbis->save();
    }

    // ogg opus
    if (auto *oggOpus = dynamic_cast<TagLib::Ogg::Opus::File*>(file_ref)) {
        auto *xc = oggOpus->tag();
        if (!xc) return false;

        xc->removeAllPictures();

        for (const auto &info : state.extracted_covers) {
            TagLib::ByteVector data = readFileToByteVector(info.temp_file_path);
            if (data.isEmpty()) continue;

            TagLib::FLAC::Picture pic;
            pic.setMimeType(info.mime_type);
            pic.setDescription(info.description);
            pic.setType(static_cast<TagLib::FLAC::Picture::Type>(info.picture_type));
            pic.setData(data);

            int w=0, h=0, d=0, c=0;
            computeImageProps(info.temp_file_path, info.mime_type, w, h, d, c);
            if (w > 0) pic.setWidth(w);
            if (h > 0) pic.setHeight(h);
            if (d > 0) pic.setColorDepth(d);
            if (c > 0) pic.setNumColors(c);

            xc->addPicture(&pic);
        }
        return oggOpus->save();
    }

    return false;
}

} // namespace chisel