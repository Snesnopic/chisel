//
// Created by Giuseppe Francione on 17/11/25.
//

#include "audio_metadata_util.hpp"
#include <array>
#include <memory>
#include <fstream>
#include <string_view>
#include <stdexcept>
#include <iterator>
#include <setjmp.h>
#include <vector>
#include <map>
#include <span>
#include <cstring>
#include <FLAC/metadata.h>
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
#include "../../include/mime_detector.hpp"
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

std::string detectMime(const TagLib::ByteVector &data) {
    return MimeDetector::detect(std::span(reinterpret_cast<const uint8_t *>(data.data()), data.size()));
}

// extracted covers are named after their content, not after the type their tag claims
std::filesystem::path coverPath(const std::filesystem::path &temp_dir, const int idx, const TagLib::ByteVector &data) {
    return temp_dir / ("cover_" + std::to_string(idx) + extFromMime(detectMime(data)));
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

// compute image props from the image's content; outputs are left untouched if it can't be read
void computeImageProps(const std::filesystem::path &imagePath,
                       int &width, int &height, int &depth, int &colors) {
    const std::string mime_type = MimeDetector::detect(imagePath);

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

        int w = 0;
        int h = 0;
        if (WebPGetInfo(header, 30, &w, &h)) {
            width = w;
            height = h;
            depth = 32; // webp is generally RGBA 8888 internally or similar
            colors = 0;
        }
        return;
    }

    // TODO: implement for jxl, etc. if needed
    Logger::log(LogLevel::Debug, "Computeimageprops: unsupported mime type: " + mime_type, "AudioMetadataUtil");
}

// flac picture fields as they were, recomputed at reinsertion when the image can be read
void keepPictureProps(AudioCoverInfo &info, const TagLib::FLAC::Picture &pic) {
    info.width = pic.width();
    info.height = pic.height();
    info.depth = pic.colorDepth();
    info.colors = pic.numColors();
}

void setPictureProps(TagLib::FLAC::Picture &pic, const AudioCoverInfo &info) {
    int w = info.width;
    int h = info.height;
    int d = info.depth;
    int c = info.colors;
    computeImageProps(info.temp_file_path, w, h, d, c);
    pic.setWidth(w);
    pic.setHeight(h);
    pic.setColorDepth(d);
    pic.setNumColors(c);
}

bool startsWith(const std::filesystem::path& file, const std::string_view magic) {
    std::ifstream in(file, std::ios::binary);
    std::string head(magic.size(), '\0');
    return in.read(head.data(), static_cast<std::streamsize>(head.size())) && head == magic;
}

// libFLAC takes UTF-8 file names on Windows too
std::string flacFileName(const std::filesystem::path& file) {
    const std::u8string name = file.u8string();
    return {name.begin(), name.end()};
}

// libFLAC rewrites the PICTURE blocks alone: TagLib would render the Vorbis comments again, sorted by key
bool rebuildFlacPictures(const std::filesystem::path& file, const std::vector<AudioCoverInfo>& covers) {
    FLAC__Metadata_Chain* chain = FLAC__metadata_chain_new();
    if (chain == nullptr) return false;
    bool ok = FLAC__metadata_chain_read(chain, flacFileName(file).c_str()) != 0;
    FLAC__Metadata_Iterator* it = ok ? FLAC__metadata_iterator_new() : nullptr;
    ok = ok && it != nullptr;
    if (ok) {
        FLAC__metadata_iterator_init(it, chain);
        std::size_t idx = 0;
        do {
            FLAC__StreamMetadata* block = FLAC__metadata_iterator_get_block(it);
            if (block->type != FLAC__METADATA_TYPE_PICTURE) continue;
            if (idx < covers.size()) {
                const AudioCoverInfo& info = covers[idx];
                TagLib::ByteVector data = readFileToByteVector(info.temp_file_path);
                if (!data.isEmpty()) {
                    int w = info.width;
                    int h = info.height;
                    int d = info.depth;
                    int c = info.colors;
                    computeImageProps(info.temp_file_path, w, h, d, c);
                    block->data.picture.width = static_cast<FLAC__uint32>(w);
                    block->data.picture.height = static_cast<FLAC__uint32>(h);
                    block->data.picture.depth = static_cast<FLAC__uint32>(d);
                    block->data.picture.colors = static_cast<FLAC__uint32>(c);
                    ok = ok && FLAC__metadata_object_picture_set_data(
                        block, reinterpret_cast<FLAC__byte*>(data.data()), data.size(), true);
                }
            }
            ++idx;
        } while (FLAC__metadata_iterator_next(it));

        // without metadata the encoder dropped the PICTURE blocks: the covers go back after the last block
        for (; ok && idx < covers.size(); ++idx) {
            const AudioCoverInfo& info = covers[idx];
            TagLib::ByteVector data = readFileToByteVector(info.temp_file_path);
            if (data.isEmpty()) continue;
            FLAC__StreamMetadata* block = FLAC__metadata_object_new(FLAC__METADATA_TYPE_PICTURE);
            if (block == nullptr) {
                ok = false;
                break;
            }
            int w = info.width;
            int h = info.height;
            int d = info.depth;
            int c = info.colors;
            computeImageProps(info.temp_file_path, w, h, d, c);
            block->data.picture.type = static_cast<FLAC__StreamMetadata_Picture_Type>(info.picture_type);
            block->data.picture.width = static_cast<FLAC__uint32>(w);
            block->data.picture.height = static_cast<FLAC__uint32>(h);
            block->data.picture.depth = static_cast<FLAC__uint32>(d);
            block->data.picture.colors = static_cast<FLAC__uint32>(c);
            std::string mime = info.mime_type;
            std::string description = info.description;
            ok = FLAC__metadata_object_picture_set_mime_type(block, mime.data(), true) &&
                 FLAC__metadata_object_picture_set_description(
                     block, reinterpret_cast<FLAC__byte*>(description.data()), true) &&
                 FLAC__metadata_object_picture_set_data(
                     block, reinterpret_cast<FLAC__byte*>(data.data()), data.size(), true) &&
                 FLAC__metadata_iterator_insert_block_after(it, block);
            if (!ok) FLAC__metadata_object_delete(block);
        }
    }
    if (it != nullptr) FLAC__metadata_iterator_delete(it);
    // padding stays as it was, so the file shrinks by what the pictures saved
    ok = ok && FLAC__metadata_chain_write(chain, false, false);
    FLAC__metadata_chain_delete(chain);
    return ok;
}

// Ogg Vorbis and Opus: only the pictures change, where TagLib would sort and upper-case every comment
bool rebuildOggPictures(TagLib::Ogg::File& file, const std::string_view header,
                        const std::vector<AudioCoverInfo>& covers) {
    const TagLib::ByteVector packet = file.packet(1);
    const auto size = static_cast<std::size_t>(packet.size());
    if (size < header.size() + 8 || std::memcmp(packet.data(), header.data(), header.size()) != 0) return false;
    std::size_t pos = header.size();
    const std::size_t vendor = packet.toUInt(static_cast<unsigned int>(pos), false);
    if (vendor > size - pos - 8) return false;
    pos += 4 + vendor;
    const unsigned int count = packet.toUInt(static_cast<unsigned int>(pos), false);
    pos += 4;

    TagLib::ByteVector rebuilt = packet.mid(0, static_cast<unsigned int>(pos));
    std::size_t picture = 0;
    for (unsigned int i = 0; i < count; ++i) {
        if (size - pos < 4) return false;
        const std::size_t length = packet.toUInt(static_cast<unsigned int>(pos), false);
        pos += 4;
        if (length > size - pos) return false;
        TagLib::ByteVector field = packet.mid(static_cast<unsigned int>(pos), static_cast<unsigned int>(length));
        pos += length;
        // the same fields TagLib reads as pictures, in the same order as the extracted covers
        if (const int sep = field.find('='); sep >= 1) {
            const TagLib::String key = TagLib::String(field.mid(0, sep), TagLib::String::UTF8).upper();
            const bool block = key == "METADATA_BLOCK_PICTURE";
            TagLib::FLAC::Picture parsed;
            if (block || key == "COVERART") {
                const TagLib::ByteVector old = TagLib::ByteVector::fromBase64(field.mid(sep + 1));
                if (!old.isEmpty() && (!block || parsed.parse(old))) {
                    const TagLib::ByteVector data =
                        picture < covers.size() ? readFileToByteVector(covers[picture].temp_file_path) : TagLib::ByteVector();
                    if (!data.isEmpty()) {
                        if (block) {
                            parsed.setData(data);
                            setPictureProps(parsed, covers[picture]);
                        }
                        field = field.mid(0, sep + 1) + (block ? parsed.render() : data).toBase64();
                    }
                    ++picture;
                }
            }
        }
        rebuilt.append(TagLib::ByteVector::fromUInt(field.size(), false));
        rebuilt.append(field);
    }
    TagLib::ByteVector rest = packet.mid(static_cast<unsigned int>(pos));
    if (header == "OpusTags") {
        // RFC 7845: what follows the comments is padding when its first byte is even
        if (!rest.isEmpty() && (rest[0] & 1) == 0) rest.clear();
    } else if (rest.size() > 1 && rest.mid(1) == TagLib::ByteVector(rest.size() - 1, '\0')) {
        // the Vorbis framing bit stays, the zero padding after it goes
        rest.resize(1);
    }
    rebuilt.append(rest);

    file.setPacket(1, rebuilt);
    // the Vorbis and Opus overrides of save() would render the comment packet again
    return file.TagLib::Ogg::File::save();
}

// RIFF (WAV) and FORM (AIFF) files keep their ID3v2 tag in an "id3 " or "ID3 " chunk
bool isChunkContainer(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::array<char, 12> head{};
    if (!in.read(head.data(), static_cast<std::streamsize>(head.size()))) return false;
    const std::string_view riff(head.data(), 4);
    const std::string_view form(head.data() + 8, 4);
    return (riff == "RIFF" && form == "WAVE") || (riff == "FORM" && (form == "AIFF" || form == "AIFC"));
}

bool copyRange(std::ifstream& in, std::ofstream& out, const uint64_t from, const uint64_t to) {
    in.clear();
    in.seekg(static_cast<std::streamoff>(from));
    std::vector<char> buffer(1 << 20);
    for (uint64_t left = to - from; left > 0;) {
        const auto n = static_cast<std::streamsize>(std::min<uint64_t>(left, buffer.size()));
        if (!in.read(buffer.data(), n)) return false;
        out.write(buffer.data(), n);
        left -= static_cast<uint64_t>(n);
    }
    return static_cast<bool>(out);
}

// the first "id3 " or "ID3 " chunk gets the new data where it is, with its size and the container's fixed
bool replaceId3Chunk(const std::filesystem::path& file, const TagLib::ByteVector& data) {
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in) return false;
    const auto fileSize = static_cast<uint64_t>(in.tellg());
    std::array<uint8_t, 12> head{};
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()))) return false;
    const bool le = std::memcmp(head.data(), "RIFF", 4) == 0;
    const auto get32 = [le](const uint8_t* b) {
        return le ? b[0] | b[1] << 8 | b[2] << 16 | static_cast<uint32_t>(b[3]) << 24
                  : static_cast<uint32_t>(b[0]) << 24 | b[1] << 16 | b[2] << 8 | b[3];
    };
    const auto put32 = [le](uint8_t* b, const uint32_t v) {
        for (int i = 0; i < 4; ++i) b[i] = static_cast<uint8_t>(v >> (le ? 8 * i : 8 * (3 - i)));
    };

    uint64_t chunk = 12;
    uint32_t oldSize = 0;
    for (std::array<uint8_t, 8> header{}; ; chunk += 8 + static_cast<uint64_t>(oldSize) + (oldSize & 1)) {
        in.seekg(static_cast<std::streamoff>(chunk));
        if (chunk + 8 > fileSize || !in.read(reinterpret_cast<char*>(header.data()), 8)) return false;
        oldSize = get32(header.data() + 4);
        if (std::memcmp(header.data(), "id3 ", 4) == 0 || std::memcmp(header.data(), "ID3 ", 4) == 0) break;
    }
    if (chunk + 8 + oldSize > fileSize) return false;
    const uint64_t oldEnd = std::min<uint64_t>(fileSize, chunk + 8 + oldSize + (oldSize & 1));
    const uint64_t newSize = data.size();
    const uint64_t container = get32(head.data() + 4) + (newSize + (newSize & 1)) - (oldEnd - chunk - 8);
    if (container > UINT32_MAX) return false;

    std::filesystem::path tmp = file;
    tmp += ".id3" + RandomUtils::random_suffix();
    bool written = false;
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        std::array<uint8_t, 4> size{};
        put32(size.data(), static_cast<uint32_t>(container));
        out.write(reinterpret_cast<const char*>(head.data()), 4);
        out.write(reinterpret_cast<const char*>(size.data()), 4);
        if (copyRange(in, out, 8, chunk + 4)) {
            put32(size.data(), static_cast<uint32_t>(newSize));
            out.write(reinterpret_cast<const char*>(size.data()), 4);
            out.write(data.data(), static_cast<std::streamsize>(data.size()));
            if (newSize & 1) out.put('\0');
            written = copyRange(in, out, oldEnd, fileSize);
        }
        out.close();
        written = written && out.good();
    }
    in.close();
    std::error_code ec;
    if (!written) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    std::filesystem::rename(tmp, file, ec);
    return !ec;
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

        const std::filesystem::path outPath = coverPath(temp_dir, idx, apic->picture());

        if (!write_file(outPath, apic->picture().data(), apic->picture().size())) {
            throw std::runtime_error("Can't write cover art to " + outPath.string());
        }

        AudioCoverInfo info;
        info.temp_file_path = outPath;
        info.mime_type = apic->mimeType().to8Bit(true);
        info.description = apic->description().to8Bit(true);
        info.picture_type = apic->type();
        info.format_specific = std::make_any<TagLib::String::Type>(apic->textEncoding());

        extracted_covers.push_back(std::move(info));
        ++idx;
    }
}

// TagLib writes only ID3v2.3 and 2.4: a 2.2 tag is closest to 2.3
TagLib::ID3v2::Version id3v2Version(const TagLib::ID3v2::Tag* tag) {
    return tag != nullptr && tag->header()->majorVersion() >= 4 ? TagLib::ID3v2::v4 : TagLib::ID3v2::v3;
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
        if (info.format_specific.has_value() && info.format_specific.type() == typeid(TagLib::String::Type)) {
            frame->setTextEncoding(std::any_cast<TagLib::String::Type>(info.format_specific));
        }
        frame->setMimeType(TagLib::String(info.mime_type, TagLib::String::UTF8));
        frame->setDescription(TagLib::String(info.description, TagLib::String::UTF8));
        frame->setType(static_cast<TagLib::ID3v2::AttachedPictureFrame::Type>(info.picture_type));
        frame->setPicture(data);

        tag->addFrame(frame);
    }
    return true;
}

// WAV and AIFF: TagLib renders the ID3v2 tag, but saving would move its chunk, bext and iXML to the end of the file
bool rebuildChunkId3Covers(const std::filesystem::path& file, const std::vector<AudioCoverInfo>& covers) {
    // TagLib keeps a pointer to the name: it has to outlive the file objects below
#ifdef _WIN32
    const std::wstring name = file.wstring();
#else
    const std::string name = file.string();
#endif
    TagLib::ByteVector rendered;
    {
        std::ifstream probe(file, std::ios::binary);
        std::array<char, 4> magic{};
        probe.read(magic.data(), static_cast<std::streamsize>(magic.size()));
        probe.close();
        std::unique_ptr<TagLib::File> taglibFile;
        TagLib::ID3v2::Tag* tag = nullptr;
        if (std::string_view(magic.data(), 4) == "RIFF") {
            auto wav = std::make_unique<TagLib::RIFF::WAV::File>(name.c_str());
            if (wav->hasID3v2Tag()) tag = wav->ID3v2Tag();
            taglibFile = std::move(wav);
        } else {
            auto aiff = std::make_unique<TagLib::RIFF::AIFF::File>(name.c_str());
            if (aiff->hasID3v2Tag()) tag = aiff->tag();
            taglibFile = std::move(aiff);
        }
        if (!taglibFile->isValid() || tag == nullptr) return false;
        const auto version = id3v2Version(tag);
        if (!rebuildId3v2Covers(tag, covers)) return false;
        rendered = tag->render(version);
    }
    return replaceId3Chunk(file, rendered);
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

            // apev2 stores no mime type
            const std::string mime = detectMime(imgData);
            const std::filesystem::path outPath = coverPath(temp_dir, idx, imgData);

            if (!write_file(outPath, imgData.data(), imgData.size())) {
                throw std::runtime_error("Can't write cover art to " + outPath.string());
            }

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
        const std::filesystem::path outPath = coverPath(temp_dir, idx, pic->data());

        if (!write_file(outPath, pic->data().data(), pic->data().size())) {
            throw std::runtime_error("Can't write cover art to " + outPath.string());
        }

        AudioCoverInfo info;
        info.temp_file_path = outPath;
        info.mime_type = pic->mimeType().to8Bit(true);
        info.description = pic->description().to8Bit(true);
        info.picture_type = normalizePictureTypeFromFlac(pic->type());
        keepPictureProps(info, *pic);

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
        pic->setMimeType(TagLib::String(info.mime_type, TagLib::String::UTF8));
        pic->setDescription(TagLib::String(info.description, TagLib::String::UTF8));
        pic->setType(static_cast<TagLib::FLAC::Picture::Type>(info.picture_type));
        pic->setData(data);

        setPictureProps(*pic, info);

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
            const std::filesystem::path outPath = coverPath(temp_dir, idx, pic->data());

            if (!write_file(outPath, pic->data().data(), pic->data().size())) {
                throw std::runtime_error("Can't write cover art to " + outPath.string());
            }

            AudioCoverInfo info;
            info.temp_file_path = outPath;
            info.mime_type = pic->mimeType().to8Bit(true);
            info.description = pic->description().to8Bit(true);
            info.picture_type = normalizePictureTypeFromFlac(pic->type());
            keepPictureProps(info, *pic);

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
                    const std::filesystem::path outPath = coverPath(temp_dir, idx, cover.data());

                    if (!write_file(outPath, cover.data().data(), cover.data().size())) {
                        throw std::runtime_error("Can't write cover art to " + outPath.string());
                    }

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
                const std::filesystem::path outPath = coverPath(temp_dir, idx, pic->data());

                if (!write_file(outPath, pic->data().data(), pic->data().size())) {
                    throw std::runtime_error("Can't write cover art to " + outPath.string());
                }

                AudioCoverInfo info;
                info.temp_file_path = outPath;
                info.mime_type = pic->mimeType().to8Bit(true);
                info.description = pic->description().to8Bit(true);
                info.picture_type = normalizePictureTypeFromFlac(pic->type());
                keepPictureProps(info, *pic);

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
                const std::filesystem::path outPath = coverPath(temp_dir, idx, pic->data());

                if (!write_file(outPath, pic->data().data(), pic->data().size())) {
                    throw std::runtime_error("Can't write cover art to " + outPath.string());
                }

                AudioCoverInfo info;
                info.temp_file_path = outPath;
                info.mime_type = pic->mimeType().to8Bit(true);
                info.description = pic->description().to8Bit(true);
                info.picture_type = normalizePictureTypeFromFlac(pic->type());
                keepPictureProps(info, *pic);

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

                if (!write_file(outPath, data.data(), data.size())) {
                    throw std::runtime_error("Can't write cover art to " + outPath.string());
                }

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
                const std::filesystem::path outPath = coverPath(temp_dir, idx, pic.picture());

                if (!write_file(outPath, pic.picture().data(), pic.picture().size())) {
                    throw std::runtime_error("Can't write cover art to " + outPath.string());
                }

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
    if (startsWith(input_path, "fLaC")) {
        return rebuildFlacPictures(input_path, state.extracted_covers);
    }
    if (isChunkContainer(input_path)) {
        return rebuildChunkId3Covers(input_path, state.extracted_covers);
    }
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
            pic->setMimeType(TagLib::String(info.mime_type, TagLib::String::UTF8));
            pic->setDescription(TagLib::String(info.description, TagLib::String::UTF8));
            pic->setType(static_cast<TagLib::FLAC::Picture::Type>(info.picture_type));
            pic->setData(data);

            // technical fields describe the *optimized* image data
            setPictureProps(*pic, info);

            flacFile->addPicture(pic);
        }
        return flacFile->save();
    }

    // mp3
    if (auto *mpegFile = dynamic_cast<TagLib::MPEG::File*>(file_ref)) {
        const auto version = id3v2Version(mpegFile->ID3v2Tag());
        if (rebuildId3v2Covers(mpegFile->ID3v2Tag(true), state.extracted_covers)) {
            // only the ID3v2 tag changed: keep its version, and any ID3v1 or APE tag as it is
            return mpegFile->save(TagLib::MPEG::File::ID3v2, TagLib::File::StripNone, version,
                                  TagLib::File::DoNotDuplicate);
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
        return rebuildOggPictures(*oggVorbis, std::string_view("\x03vorbis", 7), state.extracted_covers);
    }

    // ogg opus
    if (auto *oggOpus = dynamic_cast<TagLib::Ogg::Opus::File*>(file_ref)) {
        return rebuildOggPictures(*oggOpus, "OpusTags", state.extracted_covers);
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
        // TagLib saves a TrueAudio ID3v2 tag only as 2.4: an older tag stays as it is, covers included
        if (ttaFile->hasID3v2Tag() && id3v2Version(ttaFile->ID3v2Tag()) != TagLib::ID3v2::v4) {
            return true;
        }
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
            pic.setMimeType(TagLib::String(info.mime_type, TagLib::String::UTF8));
            pic.setDescription(TagLib::String(info.description, TagLib::String::UTF8));
            pic.setType(static_cast<TagLib::ASF::Picture::Type>(info.picture_type));
            pic.setPicture(data);

            tag->addAttribute("WM/Picture", TagLib::ASF::Attribute(pic.render()));
        }
        return asfFile->save();
    }

    // dsf
    if (auto *dsfFile = dynamic_cast<TagLib::DSF::File*>(file_ref)) {
        const auto version = id3v2Version(dsfFile->tag());
        if (rebuildId3v2Covers(dsfFile->tag(), state.extracted_covers)) {
            return dsfFile->save(version);
        }
        return false;
    }

    // dsdiff
    if (auto *dsdiffFile = dynamic_cast<TagLib::DSDIFF::File*>(file_ref)) {
        const auto version = id3v2Version(dsdiffFile->ID3v2Tag());
        if (rebuildId3v2Covers(dsdiffFile->ID3v2Tag(true), state.extracted_covers)) {
            return dsdiffFile->save(TagLib::DSDIFF::File::ID3v2, TagLib::File::StripNone, version);
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

    AudioExtractionState state;
    try {
        state = extractCovers(input_path, content.temp_dir);
    } catch (...) {
        cleanup_temp_dir(content.temp_dir, tag);
        throw;
    }

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

    // TagLib would rewrite or drop ID3v2 tags in front of a format that doesn't define them: they go back as they are
    std::vector<uint8_t> id3;
    try {
        std::filesystem::copy_file(content.original_path, final_temp_path,
                                   std::filesystem::copy_options::overwrite_existing);
        id3 = foreignId3v2Tags(final_temp_path);
        if (!id3.empty()) {
            writeWithHead(final_temp_path, final_temp_path, {}, id3.size());
        }
    } catch (const std::exception& e) {
        Logger::log(LogLevel::Error, "Failed to copy audio file: " + std::string(e.what()), tag);
        cleanup_temp_dir(content.temp_dir, tag);
        std::error_code ec;
        std::filesystem::remove(final_temp_path, ec);
        return {};
    }

    if (!rebuildCovers(final_temp_path, *state_ptr)) {
        Logger::log(LogLevel::Error, "RebuildCovers failed", tag);
        cleanup_temp_dir(content.temp_dir, tag);
        std::filesystem::remove(final_temp_path);
        return {};
    }

    if (!id3.empty()) {
        try {
            writeWithHead(final_temp_path, final_temp_path, id3, 0);
        } catch (const std::exception& e) {
            Logger::log(LogLevel::Error, "Failed to put the ID3v2 tags back: " + std::string(e.what()), tag);
            cleanup_temp_dir(content.temp_dir, tag);
            std::filesystem::remove(final_temp_path);
            return {};
        }
    }

    cleanup_temp_dir(content.temp_dir, tag);
    Logger::log(LogLevel::Debug, "Exiting finalize_extraction for " + final_temp_path.string(), tag);
    return final_temp_path;
}

std::vector<uint8_t> AudioMetadataUtil::foreignId3v2Tags(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::array<uint8_t, 10> header{};
    std::size_t end = 0;
    while (in.seekg(static_cast<std::streamoff>(end)) && in.read(reinterpret_cast<char*>(header.data()), 10)) {
        if (std::memcmp(header.data(), "ID3", 3) != 0 || header[3] < 2 || header[3] > 4 || header[4] == 0xff) {
            break;
        }
        std::size_t size = 0;
        bool syncsafe = true;
        for (std::size_t i = 6; i < 10; ++i) {
            syncsafe = syncsafe && (header[i] & 0x80) == 0;
            size = (size << 7) | header[i];
        }
        if (!syncsafe) {
            break;
        }
        const bool footer = header[3] == 4 && (header[5] & 0x10) != 0;
        end += 10 + size + (footer ? 10 : 0);
    }
    if (end == 0) {
        return {};
    }

    // zero padding some taggers leave outside the tag's declared size stays with the tags
    in.clear();
    in.seekg(static_cast<std::streamoff>(end));
    for (int c = in.get(); c == 0; c = in.get()) {
        ++end;
    }

    // MPEG audio and TrueAudio define their ID3v2 tag: these formats only put up with one in front
    std::array<char, 4> magic{};
    in.clear();
    in.seekg(static_cast<std::streamoff>(end));
    in.read(magic.data(), magic.size());
    const std::string_view format(magic.data(), static_cast<std::size_t>(in.gcount()));
    if (format != "fLaC" && format != "MAC " && format != "wvpk" && format != "MPCK" && !format.starts_with("MP+")) {
        return {};
    }
    std::vector<uint8_t> tags(end);
    in.clear();
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(tags.data()), static_cast<std::streamsize>(end))) {
        return {};
    }
    return tags;
}

void AudioMetadataUtil::writeWithHead(const std::filesystem::path& src, const std::filesystem::path& dst,
                                      const std::span<const uint8_t> head, const std::size_t skip) {
    // written next to dst first, so that src can be dst itself
    std::filesystem::path tmp = dst;
    tmp += ".head" + RandomUtils::random_suffix();
    bool written = false;
    {
        std::ifstream in(src, std::ios::binary);
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (in && out) {
            out.write(reinterpret_cast<const char*>(head.data()), static_cast<std::streamsize>(head.size()));
            in.seekg(static_cast<std::streamoff>(skip));
            std::vector<char> buffer(1 << 20);
            while (in.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) || in.gcount() > 0) {
                out.write(buffer.data(), in.gcount());
            }
            out.close();
            written = !in.bad() && out.good();
        }
    }
    if (!written) {
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        throw std::runtime_error("Can't rewrite " + dst.string());
    }
    std::filesystem::rename(tmp, dst);
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