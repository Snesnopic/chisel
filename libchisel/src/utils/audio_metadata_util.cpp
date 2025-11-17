//
// Created by Giuseppe Francione on 17/11/25.
//

#include "audio_metadata_util.hpp"
#include <fstream>
#include <iterator>
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

namespace chisel {

// helpers
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
    return ".jpg";
}

// infer mp4 format from mime
inline TagLib::MP4::CoverArt::Format inferFormatFromMime(const std::string &mime) {
    return (mime == "image/png") ? TagLib::MP4::CoverArt::PNG : TagLib::MP4::CoverArt::JPEG;
}

// compute image props
inline void computeImageProps(const std::filesystem::path &imagePath,
                              int &width, int &height, int &depth, int &colors) {
    // TODO: implement image property extraction with libjpeg/libpng/libjxl
    width = 0;
    height = 0;
    depth = 0;
    colors = 0;
}

// normalize picture type flac
inline int normalizePictureTypeFromFlac(const TagLib::FLAC::Picture::Type t) {
    return static_cast<int>(t);
}

// default front cover type
inline int defaultFrontCoverType() {
    return 3;
}

} // namespace

// extraction
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

            // TODO: decide if store flac picture fields or compute later

            state.extracted_covers.push_back(std::move(info));
            ++idx;
        }
        return state;
    }

    // mp3
    if (auto *mpegFile = dynamic_cast<TagLib::MPEG::File*>(file_ref)) {
        auto *id3v2tag = mpegFile->ID3v2Tag();
        if (id3v2tag) {
            int idx = 0;
            auto frames = id3v2tag->frameList("APIC");
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

                state.extracted_covers.push_back(std::move(info));
                ++idx;
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
                    info.description = "";
                    info.picture_type = defaultFrontCoverType();
                    info.format_specific = cover.format();

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

    // TODO: add support for ape and wav containers
    return state;
}

// reinsertion
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

            TagLib::FLAC::Picture pic;
            pic.setMimeType(info.mime_type);
            pic.setDescription(info.description);
pic.setType(static_cast<TagLib::FLAC::Picture::Type>(info.picture_type));
            pic.setData(data);

            int w=0, h=0, d=0, c=0;
            computeImageProps(info.temp_file_path, w, h, d, c);
            if (w > 0) pic.setWidth(w);
            if (h > 0) pic.setHeight(h);
            if (d > 0) pic.setColorDepth(d);
            if (c > 0) pic.setNumColors(c);

            flacFile->addPicture(&pic);
        }
        return flacFile->save();
    }

    // mp3
    if (auto *mpegFile = dynamic_cast<TagLib::MPEG::File*>(file_ref)) {
        auto *id3v2tag = mpegFile->ID3v2Tag(true);
        id3v2tag->removeFrames("APIC");

        for (const auto &info : state.extracted_covers) {
            TagLib::ByteVector data = readFileToByteVector(info.temp_file_path);

            auto *frame = new TagLib::ID3v2::AttachedPictureFrame;
            [[maybe_unused]] auto type = frame->type();
            frame->setMimeType(info.mime_type);
            frame->setDescription(info.description);
            frame->setType(static_cast<decltype(type)>(info.picture_type));
            frame->setPicture(data);

            id3v2tag->addFrame(frame);
        }
        return mpegFile->save();
    }

    // mp4
    if (auto *mp4File = dynamic_cast<TagLib::MP4::File*>(file_ref)) {
        TagLib::MP4::CoverArtList covers;

        for (const auto &info : state.extracted_covers) {
            TagLib::ByteVector data = readFileToByteVector(info.temp_file_path);

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
        tag->setItem("covr", covers);

        return mp4File->save();
    }

    // ogg vorbis
    if (auto *oggVorbis = dynamic_cast<TagLib::Ogg::Vorbis::File*>(file_ref)) {
        auto *xc = oggVorbis->tag();
        if (!xc) return false;

        xc->removeAllPictures();

        for (const auto &info : state.extracted_covers) {
            TagLib::ByteVector data = readFileToByteVector(info.temp_file_path);

            TagLib::FLAC::Picture pic;
            pic.setMimeType(info.mime_type);
            pic.setDescription(info.description);
            pic.setType(static_cast<TagLib::FLAC::Picture::Type>(info.picture_type));
            pic.setData(data);

            int w=0, h=0, d=0, c=0;
            computeImageProps(info.temp_file_path, w, h, d, c);
            if (w > 0) pic.setWidth(w);
            if (h > 0) pic.setHeight(h);
            if (d > 0) pic.setColorDepth(d);
            if (c > 0) pic.setNumColors(c);

            xc->addPicture(&pic);
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

            TagLib::FLAC::Picture pic;
            pic.setMimeType(info.mime_type);
            pic.setDescription(info.description);
            pic.setType(static_cast<TagLib::FLAC::Picture::Type>(info.picture_type));
            pic.setData(data);

            int w=0, h=0, d=0, c=0;
            computeImageProps(info.temp_file_path, w, h, d, c);
            if (w > 0) pic.setWidth(w);
            if (h > 0) pic.setHeight(h);
            if (d > 0) pic.setColorDepth(d);
            if (c > 0) pic.setNumColors(c);

            xc->addPicture(&pic);
        }
        return oggOpus->save();
    }

    // TODO: implement reinsertion for ape and wav formats
    return false;
}

} // namespace chisel