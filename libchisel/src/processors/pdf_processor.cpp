//
// Created by Giuseppe Francione on 19/10/25.
//

#include "../../include/pdf_processor.hpp"
#include "../../include/file_type.hpp"
#include "../../include/logger.hpp"
#include "../../include/file_utils.hpp"
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFWriter.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/Buffer.hh>
#include <qpdf/QPDFLogger.hh>
#include <qpdf/QPDFCryptoProvider.hh>
#include <qpdf/QPDFEmbeddedFileDocumentHelper.hh>
#include <qpdf/Pl_Flate.hh>
#include <qpdf/Pl_OStream.hh>
#include <fstream>
#include <sstream>
#include <map>
#include <memory>
#include <numeric>
#include <mutex>
#include <string>
#include <vector>
#include <algorithm>
#include <array>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string_view>
#include <zlib.h>
#include "zlib_container.h"
#include "zopfli.h"
#include "zopfli_compressor.hpp"
#include "../../include/thread_pool.hpp"

namespace chisel {

namespace {
    // provides raw, pre-compressed data to qpdf, bypassing internal zlib/encoders
    class raw_stream_provider : public QPDFObjectHandle::StreamDataProvider {
        std::vector<unsigned char> data_;
    public:
        explicit raw_stream_provider(std::vector<unsigned char> data) : data_(std::move(data)) {}

        void provideStreamData(int /*objid*/, int /*gen*/, Pipeline* pipeline) override {
            pipeline->write(data_.data(), data_.size());
            pipeline->finish();
        }

        // forces qpdf to write the stream exactly as provided
        static bool supportsCompression() {
            return false;
        }
    };
/**
 * @brief A custom std::stringbuf that redirects its content to the chisel Logger.
 * This is used to capture warnings and errors from QPDF.
 */
struct LoggerStreamBuf final : std::stringbuf {
    LogLevel level;
    std::string module;
    LoggerStreamBuf(const LogLevel lvl, const char* mod) : level(lvl), module(mod) {}
    ~LoggerStreamBuf() override { LoggerStreamBuf::sync(); }
    protected:
    int sync() noexcept override {
        const std::string s = str();
        if (!s.empty()) {
            Logger::log(level, s, module);
            str("");
        }
        return 0;
    }
};

/**
 * @brief File names of the embedded file streams listed in the document's /EmbeddedFiles tree.
 */
std::map<QPDFObjGen, std::string> embedded_file_names(QPDF& pdf) {
    std::map<QPDFObjGen, std::string> names;
    QPDFEmbeddedFileDocumentHelper helper(pdf);
    for (const auto& [key, spec] : helper.getEmbeddedFiles()) {
        const QPDFObjectHandle stream = spec->getEmbeddedFileStream();
        if (stream.isStream()) names.emplace(stream.getObjGen(), spec->getFilename());
    }
    return names;
}

/**
 * @brief Extension for a stream that holds a whole file, which is extracted for the executor
 *        to detect and process.
 * @param decodable Whether the stream's data could be fully decoded.
 * @param names File names of embedded file streams.
 * @return The extension, or an empty string for data that is only meaningful inside the PDF
 *         (content streams, raw pixels, fonts...), which finalize_extraction recompresses itself.
 */
std::string file_extension(QPDFObjectHandle const& stream, const bool decodable,
                           const std::map<QPDFObjGen, std::string>& names) {
    const QPDFObjectHandle dict = stream.getDict();
    if (!dict.isDictionary()) return {};

    // image codecs aren't decoded; behind another filter the result couldn't be injected back as is
    QPDFObjectHandle filter = dict.getKey("/Filter");
    if (filter.isArray() && filter.getArrayNItems() == 1) filter = filter.getArrayItem(0);
    if (filter.isNameAndEquals("/DCTDecode")) return ".jpg";
    if (filter.isNameAndEquals("/JPXDecode")) return ".jp2";

    if (!decodable) return {};
    const QPDFObjectHandle type = dict.getKey("/Type");
    if (type.isNameAndEquals("/Metadata")) return ".xml";
    if (type.isNameAndEquals("/EmbeddedFile")) {
        const auto it = names.find(stream.getObjGen());
        const auto extension = it != names.end() ? std::filesystem::path(it->second).extension().string() : "";
        return extension.empty() ? ".bin" : extension;
    }
    return {};
}

/**
 * @brief Keeps an embedded file's /Params /Size and /CheckSum (MD5) in line with its new content.
 * @param content The file holding the new decoded content.
 */
void update_embedded_file_params(QPDFObjectHandle const& stream, const std::filesystem::path& content) {
    const QPDFObjectHandle dict = stream.getDict();
    if (!dict.getKey("/Type").isNameAndEquals("/EmbeddedFile")) return;
    QPDFObjectHandle params = dict.getKey("/Params");
    if (!params.isDictionary()) return;
    std::vector<uint8_t> data;
    if (!read_file(content, data)) return;
    if (params.hasKey("/Size")) {
        params.replaceKey("/Size", QPDFObjectHandle::newInteger(static_cast<long long>(data.size())));
    }
    if (params.hasKey("/CheckSum")) {
        const auto md5 = QPDFCryptoProvider::getImpl();
        QPDFCryptoImpl::MD5_Digest digest;
        md5->MD5_init();
        md5->MD5_update(data.data(), data.size());
        md5->MD5_finalize();
        md5->MD5_digest(digest);
        params.replaceKey("/CheckSum", QPDFObjectHandle::newString(std::string(reinterpret_cast<const char*>(digest), sizeof digest)));
    }
}

/**
 * @brief Which single, lossless, general-purpose filter (if any) a PDF
 * stream uses. LZWDecode/ASCII85Decode/ASCIIHexDecode are filters qpdf's own
 * CLI would recompress under --decode-level=generalized; chisel doesn't call
 * that CLI, so this exists to single them out from image-specific filters
 * (DCTDecode/JPXDecode, which are lossy to re-encode) and multi-filter or
 * unrecognized cases, which this code doesn't attempt to handle.
 */
enum class SingleFilterKind { None, Flate, Lzw, Ascii85, AsciiHex };

/**
 * @brief Classifies a PDF stream's filter, if it has exactly one.
 * @param stream The QPDFObjectHandle for the stream.
 * @return The filter kind, or SingleFilterKind::None if the stream has zero,
 * more than one, or an unrecognized filter.
 */
SingleFilterKind classify_single_filter(QPDFObjectHandle const& stream) {
    if (!stream.isStream()) return SingleFilterKind::None;
    const QPDFObjectHandle dict = stream.getDict();
    if (!dict.isDictionary()) return SingleFilterKind::None;
    QPDFObjectHandle filter = dict.getKey("/Filter");
    if (filter.isArray()) {
        if (filter.getArrayNItems() != 1) return SingleFilterKind::None;
        filter = filter.getArrayItem(0);
    }
    if (!filter.isName()) return SingleFilterKind::None;
    const std::string name = filter.getName();
    if (name == "/FlateDecode") return SingleFilterKind::Flate;
    if (name == "/LZWDecode") return SingleFilterKind::Lzw;
    if (name == "/ASCII85Decode") return SingleFilterKind::Ascii85;
    if (name == "/ASCIIHexDecode") return SingleFilterKind::AsciiHex;
    return SingleFilterKind::None;
}

// zopflipng's own established convention for splitting effort between the
// "iterations" and "iterations_large" budgets; reused here for the same
// reason it exists there -- full effort on small streams is cheap, but
// unbounded effort on a multi-megabyte one is not.
constexpr size_t kLargeStreamThreshold = 200000;

unsigned pick_iterations(const size_t data_size, const ProcessingOptions& options) {
    return static_cast<unsigned>(data_size < kLargeStreamThreshold ? options.iterations : options.iterations_large);
}

void set_env_var(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

/**
 * @brief Turns on qpdf's own internal zopfli use, for the object/xref
 * streams QPDFWriter generates itself (chisel's own stream recompression
 * bypasses qpdf's compressor entirely via raw_stream_provider, so this
 * doesn't affect those).
 *
 * @details qpdf's Pl_Flate::zopfli_enabled() caches its QPDF_ZOPFLI check in
 * a function-local static the first time it's asked, during the first
 * a_deflate use in the whole process -- so this must run before the first
 * QPDFWriter::write() call anywhere in the process, and setting the
 * environment variable again later has no effect. PdfProcessor is a single
 * shared instance across every PDF in a run (see ProcessorRegistry), so this
 * runs at most once, on whichever PDF's finalize_extraction() reaches it
 * first; iterations is a run-wide setting, so using the first file's value
 * for the whole run is correct, not just convenient.
 */
void enable_qpdf_zopfli_once(const ProcessingOptions& options) {
    static std::once_flag flag;
    std::call_once(flag, [&options]() {
        if (Pl_Flate::zopfli_supported()) {
            set_env_var("QPDF_ZOPFLI", "force");
            // qpdf's own streams (object/xref streams) are structural
            // bookkeeping, not big embedded content, so the full iterations
            // budget is always appropriate here -- unlike the per-stream
            // choice in finalize_extraction() for the streams chisel
            // compresses itself.
            Pl_Flate::setZopfliIterations(static_cast<int>(options.iterations));
        }
    });
}

// inflates a whole zlib stream, or nothing unless it ends cleanly, checksum included
std::optional<std::vector<unsigned char>> inflate_zlib(const unsigned char* data, const std::size_t size) {
    if (size > UINT_MAX) return std::nullopt;
    z_stream zs{};
    if (inflateInit(&zs) != Z_OK) return std::nullopt;
    zs.next_in = const_cast<Bytef*>(data);
    zs.avail_in = static_cast<uInt>(size);
    std::vector<unsigned char> out;
    std::array<unsigned char, 1 << 16> chunk{};
    int rc = Z_OK;
    while (rc == Z_OK) {
        zs.next_out = chunk.data();
        zs.avail_out = static_cast<uInt>(chunk.size());
        rc = inflate(&zs, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END) break;
        out.insert(out.end(), chunk.data(), chunk.data() + (chunk.size() - zs.avail_out));
        // input used up with the output buffer not full: the stream is truncated
        if (rc == Z_OK && zs.avail_in == 0 && zs.avail_out != 0) break;
    }
    inflateEnd(&zs);
    if (rc != Z_STREAM_END) return std::nullopt;
    return out;
}

// raw image samples travel through the png pipeline and come back as png-predicted flate data
namespace pdf_image {

// bigger images keep the plain recompression: the png pipeline holds several copies of them in memory
constexpr std::uint64_t kMaxSamples = std::uint64_t{64} << 20;
// smaller ones can't repay the /DecodeParms entry a predictor needs
constexpr std::uint64_t kMinSamples = 64;

constexpr std::array<unsigned char, 8> kSignature = {137, 80, 78, 71, 13, 10, 26, 10};

// samples per pixel of an image color space, 0 when it can't be told
int color_components(const QPDFObjectHandle& space) {
    if (space.isName()) {
        const std::string name = space.getName();
        if (name == "/DeviceGray") return 1;
        if (name == "/DeviceRGB") return 3;
        if (name == "/DeviceCMYK") return 4;
        return 0;
    }
    if (!space.isArray() || space.getArrayNItems() < 2 || !space.getArrayItem(0).isName()) return 0;
    const std::string family = space.getArrayItem(0).getName();
    if (family == "/Indexed" || family == "/Separation" || family == "/CalGray") return 1;
    if (family == "/CalRGB" || family == "/Lab") return 3;
    if (family == "/ICCBased") {
        const QPDFObjectHandle profile = space.getArrayItem(1);
        if (!profile.isStream()) return 0;
        const QPDFObjectHandle n = profile.getDict().getKey("/N");
        return n.isInteger() ? n.getIntValueAsInt() : 0;
    }
    if (family == "/DeviceN") {
        const QPDFObjectHandle names = space.getArrayItem(1);
        return names.isArray() ? names.getArrayNItems() : 0;
    }
    return 0;
}

std::uint64_t row_size(const PdfImageLayout& image) {
    return (static_cast<std::uint64_t>(image.width) * static_cast<std::uint64_t>(image.colors) *
            static_cast<std::uint64_t>(image.bits) + 7) / 8;
}

/**
 * @brief The layout of an image whose samples fit a PNG, which allows 1 to 16-bit gray but only
 *        8 and 16-bit samples with more channels.
 * @param samples Size of the samples qpdf decoded, which must be exactly the declared ones.
 */
std::optional<PdfImageLayout> layout_of(QPDFObjectHandle const& stream, const std::size_t samples) {
    const QPDFObjectHandle dict = stream.getDict();
    if (!dict.isDictionary() || !dict.getKey("/Subtype").isNameAndEquals("/Image")) return std::nullopt;
    const QPDFObjectHandle width = dict.getKey("/Width");
    const QPDFObjectHandle height = dict.getKey("/Height");
    if (!width.isInteger() || !height.isInteger()) return std::nullopt;
    PdfImageLayout image{.width = width.getIntValueAsInt(), .height = height.getIntValueAsInt()};
    if (const QPDFObjectHandle mask = dict.getKey("/ImageMask"); mask.isBool() && mask.getBoolValue()) {
        image.colors = 1;
        image.bits = 1;
    } else {
        const QPDFObjectHandle bits = dict.getKey("/BitsPerComponent");
        if (!bits.isInteger()) return std::nullopt;
        image.colors = color_components(dict.getKey("/ColorSpace"));
        image.bits = bits.getIntValueAsInt();
    }
    const bool gray = image.colors == 1 &&
                      (image.bits == 1 || image.bits == 2 || image.bits == 4 || image.bits == 8 || image.bits == 16);
    const bool color = image.colors >= 2 && image.colors <= 4 && (image.bits == 8 || image.bits == 16);
    if (image.width <= 0 || image.height <= 0 || (!gray && !color)) return std::nullopt;
    const std::uint64_t size = row_size(image) * static_cast<std::uint64_t>(image.height);
    if (size != samples || size < kMinSamples || size > kMaxSamples) return std::nullopt;
    return image;
}

unsigned char color_type(const int colors) {
    switch (colors) {
        case 1: return 0;
        case 2: return 4;
        case 3: return 2;
        default: return 6;
    }
}

void put_u32(std::vector<unsigned char>& out, const std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<unsigned char>(value >> shift));
}

std::uint32_t get_u32(const unsigned char* p) {
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | std::uint32_t{p[3]};
}

void append_chunk(std::vector<unsigned char>& png, const std::string_view type, const std::vector<unsigned char>& data) {
    put_u32(png, static_cast<std::uint32_t>(data.size()));
    const std::size_t start = png.size();
    png.insert(png.end(), type.begin(), type.end());
    png.insert(png.end(), data.begin(), data.end());
    put_u32(png, static_cast<std::uint32_t>(crc32(0, png.data() + start, static_cast<uInt>(png.size() - start))));
}

// the samples as a png of unfiltered, cheaply deflated rows: the png pipeline redoes both
std::optional<std::vector<unsigned char>> to_png(const PdfImageLayout& image, const std::vector<unsigned char>& samples) {
    const auto row = static_cast<std::size_t>(row_size(image));
    const auto height = static_cast<std::size_t>(image.height);
    std::vector<unsigned char> rows;
    rows.reserve((row + 1) * height);
    for (std::size_t y = 0; y < height; ++y) {
        rows.push_back(0);
        const auto begin = samples.begin() + static_cast<std::ptrdiff_t>(y * row);
        rows.insert(rows.end(), begin, begin + static_cast<std::ptrdiff_t>(row));
    }
    uLongf size = compressBound(static_cast<uLong>(rows.size()));
    std::vector<unsigned char> idat(size);
    if (compress2(idat.data(), &size, rows.data(), static_cast<uLong>(rows.size()), 1) != Z_OK) return std::nullopt;
    idat.resize(size);

    std::vector<unsigned char> header;
    put_u32(header, static_cast<std::uint32_t>(image.width));
    put_u32(header, static_cast<std::uint32_t>(image.height));
    header.insert(header.end(), {static_cast<unsigned char>(image.bits), color_type(image.colors), 0, 0, 0});

    std::vector<unsigned char> png(kSignature.begin(), kSignature.end());
    append_chunk(png, "IHDR", header);
    append_chunk(png, "IDAT", idat);
    append_chunk(png, "IEND", {});
    return png;
}

// the zlib data of a png that kept the image's layout, without interlacing
std::optional<std::vector<unsigned char>> image_data(const std::vector<unsigned char>& png, const PdfImageLayout& image) {
    if (png.size() < kSignature.size() || !std::equal(kSignature.begin(), kSignature.end(), png.begin())) {
        return std::nullopt;
    }
    std::vector<unsigned char> data;
    bool header = false;
    for (std::size_t pos = kSignature.size(); pos + 12 <= png.size();) {
        const std::uint32_t length = get_u32(&png[pos]);
        if (length > png.size() - pos - 12) return std::nullopt;
        const std::string_view type(reinterpret_cast<const char*>(&png[pos + 4]), 4);
        const unsigned char* body = &png[pos + 8];
        if (type == "IHDR") {
            header = length == 13 && get_u32(body) == static_cast<std::uint32_t>(image.width) &&
                     get_u32(body + 4) == static_cast<std::uint32_t>(image.height) && body[8] == image.bits &&
                     body[9] == color_type(image.colors) && body[10] == 0 && body[11] == 0 && body[12] == 0;
            if (!header) return std::nullopt;
        } else if (type == "IDAT") {
            if (!header) return std::nullopt;
            data.insert(data.end(), body, body + length);
        } else if (type == "IEND") {
            break;
        }
        pos += 12 + static_cast<std::size_t>(length);
    }
    if (!header || data.empty()) return std::nullopt;
    return data;
}

// undoes the png row filters; nothing unless every row names a known filter
std::optional<std::vector<unsigned char>> unfilter(const std::vector<unsigned char>& rows, const PdfImageLayout& image) {
    const auto row = static_cast<std::size_t>(row_size(image));
    const auto height = static_cast<std::size_t>(image.height);
    if (rows.size() != (row + 1) * height) return std::nullopt;
    const auto bpp = static_cast<std::size_t>(std::max(1, image.colors * image.bits / 8));
    std::vector<unsigned char> out(row * height);
    for (std::size_t y = 0; y < height; ++y) {
        const unsigned char type = rows[y * (row + 1)];
        const unsigned char* in = &rows[y * (row + 1) + 1];
        unsigned char* cur = &out[y * row];
        const unsigned char* prev = y > 0 ? cur - row : nullptr;
        for (std::size_t x = 0; x < row; ++x) {
            const int a = x >= bpp ? cur[x - bpp] : 0;
            const int b = prev ? prev[x] : 0;
            const int c = prev && x >= bpp ? prev[x - bpp] : 0;
            int predicted;
            switch (type) {
                case 0: predicted = 0; break;
                case 1: predicted = a; break;
                case 2: predicted = b; break;
                case 3: predicted = (a + b) / 2; break;
                case 4: {
                    const int p = a + b - c;
                    const int pa = std::abs(p - a);
                    const int pb = std::abs(p - b);
                    const int pc = std::abs(p - c);
                    predicted = pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
                    break;
                }
                default: return std::nullopt;
            }
            cur[x] = static_cast<unsigned char>(in[x] + predicted);
        }
    }
    return out;
}

QPDFObjectHandle predictor_parms(const PdfImageLayout& image) {
    QPDFObjectHandle parms = QPDFObjectHandle::newDictionary();
    parms.replaceKey("/Predictor", QPDFObjectHandle::newInteger(15));
    // one color, 8 bits and one column are the defaults
    if (image.colors != 1) parms.replaceKey("/Colors", QPDFObjectHandle::newInteger(image.colors));
    if (image.bits != 8) parms.replaceKey("/BitsPerComponent", QPDFObjectHandle::newInteger(image.bits));
    if (image.width != 1) parms.replaceKey("/Columns", QPDFObjectHandle::newInteger(image.width));
    return parms;
}

// what a /DecodeParms entry adds to the stream dictionary, which is written outside object streams
std::size_t parms_cost(QPDFObjectHandle parms) {
    return parms.isNull() ? 0 : std::string_view(" /DecodeParms ").size() + parms.unparse().size();
}

/**
 * @brief An image stream as smallest_encoding() needs it, read from qpdf beforehand.
 */
struct ImageInput {
    PdfImageLayout image;
    std::vector<unsigned char> samples;
    std::vector<unsigned char> raw;   ///< The stream as it is, kept when its own predictor rows are a candidate
    std::size_t cost = 0;             ///< What the stream costs as it is, its /DecodeParms included
    std::size_t own_parms_cost = 0;   ///< What its own /DecodeParms adds to the rows candidate
    std::size_t predictor_parms_cost = 0;
    std::filesystem::path png_file;   ///< The PNG the samples went out as
    uintmax_t png_size = 0;           ///< Its size as written
};

enum class Parms { None, Own, Predictor };

struct Encoding {
    std::vector<unsigned char> data;
    Parms parms = Parms::None;
};

/**
 * @brief Reads what smallest_encoding() needs from an image stream.
 * @param own_parms Set to the stream's own /DecodeParms when its rows are a candidate.
 */
ImageInput read_image(QPDFObjectHandle& stream, const PdfImageLayout& image, const std::filesystem::path& png_file,
                      const uintmax_t png_size, QPDFObjectHandle& own_parms) {
    ImageInput in{.image=image, .png_file=png_file, .png_size=png_size};
    const std::shared_ptr<Buffer> raw = stream.getRawStreamData();
    const std::shared_ptr<Buffer> decoded = stream.getStreamData(qpdf_dl_specialized);
    in.samples.assign(decoded->getBuffer(), decoded->getBuffer() + decoded->getSize());

    const QPDFObjectHandle dict = stream.getDict();
    in.cost = raw->getSize() + parms_cost(dict.getKey("/DecodeParms"));
    in.predictor_parms_cost = parms_cost(predictor_parms(image));
    if (classify_single_filter(stream) == SingleFilterKind::Flate && dict.hasKey("/DecodeParms")) {
        QPDFObjectHandle parms = dict.getKey("/DecodeParms");
        if (parms.isArray() && parms.getArrayNItems() == 1) parms = parms.getArrayItem(0);
        if (parms.isDictionary()) {
            in.raw.assign(raw->getBuffer(), raw->getBuffer() + raw->getSize());
            in.own_parms_cost = parms_cost(parms);
            own_parms = parms;
        }
    }
    return in;
}

/**
 * @brief The smallest Flate encoding of an image's samples, if it beats the stream as it is: the samples
 *        deflated as they are, the rows its own predictor made, or the rows the PNG pipeline filtered.
 */
std::optional<Encoding> smallest_encoding(const ImageInput& in, const ProcessingOptions& options) {
    std::optional<Encoding> best;
    std::size_t best_cost = in.cost;
    const auto consider = [&](std::vector<unsigned char> data, const std::size_t parms_cost, const Parms parms) {
        const std::size_t cost = data.size() + parms_cost;
        if (cost >= best_cost) return;
        best_cost = cost;
        best = Encoding{std::move(data), parms};
    };

    consider(ZopfliCompressor::compress(in.samples, pick_iterations(in.samples.size(), options), ZopfliFormat::ZLIB),
             0, Parms::None);

    if (!in.raw.empty()) {
        if (const auto rows = inflate_zlib(in.raw.data(), in.raw.size())) {
            consider(ZopfliCompressor::compress(*rows, pick_iterations(rows->size(), options), ZopfliFormat::ZLIB),
                     in.own_parms_cost, Parms::Own);
        }
    }

    // only what the png pipeline shrank, and only if it decodes back to the very same samples
    std::error_code ec;
    std::vector<unsigned char> png;
    if (const auto size = std::filesystem::file_size(in.png_file, ec);
        ec || size >= in.png_size || !read_file(in.png_file, png)) {
        return best;
    }
    auto data = image_data(png, in.image);
    const auto rows = data ? inflate_zlib(data->data(), data->size()) : std::nullopt;
    if (const auto restored = rows ? unfilter(*rows, in.image) : std::nullopt; restored && *restored == in.samples) {
        consider(std::move(*data), in.predictor_parms_cost, Parms::Predictor);
    } else {
        Logger::log(LogLevel::Warning, in.png_file.filename().string() + " came back with another layout or other samples, not used",
                    "PdfProcessor");
    }
    return best;
}

} // namespace pdf_image

// streams read ahead of their compression, at most this much at a time
constexpr std::size_t kStreamBatchBytes = std::size_t{256} << 20;

/**
 * @brief One stream's recompression. Its inputs are read from qpdf before and its result is applied after, on
 *        the thread that owns the QPDF object, so that only the compression runs elsewhere.
 */
struct StreamJob {
    enum class Kind {
        Image,     ///< samples that went out as a PNG
        Optimized, ///< a stream whose extracted file came back smaller
        Plain,     ///< a Flate/LZW/ASCII85/ASCIIHex stream without /DecodeParms
        Predicted  ///< a Flate stream with /DecodeParms, deflated again as it inflates
    };
    Kind kind = Kind::Plain;
    QPDFObjectHandle obj;              ///< Only touched on the QPDF object's thread
    int obj_id = 0;
    std::vector<unsigned char> data;   ///< The file, the decoded stream, or the raw stream to inflate
    std::size_t raw_size = 0;          ///< Plain and predicted: the stream as it is, to beat
    bool deflate = false;              ///< Optimized: whether the file goes back deflated
    bool to_flate = false;             ///< Plain: whether the filter becomes /FlateDecode
    std::filesystem::path file;        ///< Optimized: the file it came back as
    pdf_image::ImageInput image;
    QPDFObjectHandle own_parms;        ///< Image: its own /DecodeParms, for its rows candidate
    std::optional<std::vector<unsigned char>> result;
    pdf_image::Parms parms = pdf_image::Parms::None;

    [[nodiscard]] std::size_t bytes() const { return data.size() + image.samples.size() + image.raw.size(); }
};

/**
 * @brief Computes a job's new stream data, if it has any; touches no qpdf object.
 */
void compress_stream(StreamJob& job, const ProcessingOptions& options) {
    const auto deflate = [&options](const std::vector<unsigned char>& in) {
        return ZopfliCompressor::compress(in, pick_iterations(in.size(), options), ZopfliFormat::ZLIB);
    };
    const std::string obj = std::to_string(job.obj_id);
    switch (job.kind) {
        case StreamJob::Kind::Image:
            try {
                if (auto encoding = pdf_image::smallest_encoding(job.image, options)) {
                    job.result = std::move(encoding->data);
                    job.parms = encoding->parms;
                }
            } catch (const std::exception& e) {
                Logger::log(LogLevel::Debug, "Image obj " + obj + " left as is: " + e.what(), "PdfProcessor");
            }
            break;
        case StreamJob::Kind::Optimized:
            // the extracted file holds *decoded* content (e.g. a full PNG file
            // guessed from a FlateDecode Image stream); the original filter is
            // being kept on the dict, so the bytes must be re-compressed to stay
            // valid under it. DCTDecode/JPXDecode extractions are already in their
            // final on-disk form and need no re-wrapping.
            if (!job.deflate) {
                job.result = std::move(job.data);
                break;
            }
            try {
                job.result = deflate(job.data);
            } catch (const std::exception& e) {
                Logger::log(LogLevel::Warning, "Failed to re-compress optimized stream for obj " + obj +
                            ", skipping injection: " + e.what(), "PdfProcessor");
            }
            break;
        case StreamJob::Kind::Plain:
            try {
                if (auto recompressed = deflate(job.data); recompressed.size() < job.raw_size) {
                    job.result = std::move(recompressed);
                }
            } catch (const std::exception&) {
                Logger::log(LogLevel::Debug, "zopfli skipped on obj " + obj, "PdfProcessor");
            }
            break;
        case StreamJob::Kind::Predicted:
            try {
                if (const auto inflated = inflate_zlib(job.data.data(), job.data.size())) {
                    if (auto recompressed = deflate(*inflated); recompressed.size() < job.raw_size) {
                        job.result = std::move(recompressed);
                    }
                }
            } catch (const std::exception&) {
                Logger::log(LogLevel::Debug, "zopfli skipped on obj " + obj, "PdfProcessor");
            }
            break;
    }
    job.data = {};
    job.image.samples = {};
    job.image.raw = {};
}

/**
 * @brief Puts a job's new stream data in its stream, keeping the stream's filters unless the job changed them.
 */
void apply_stream(StreamJob& job) {
    if (!job.result || job.result->empty()) return;
    auto provider = std::make_shared<raw_stream_provider>(std::move(*job.result));
    const QPDFObjectHandle dict = job.obj.getDict();
    switch (job.kind) {
        case StreamJob::Kind::Image: {
            // a /Filter name, not an array, is what makes QPDFWriter keep the data as it is
            QPDFObjectHandle parms = QPDFObjectHandle::newNull();
            if (job.parms == pdf_image::Parms::Own) parms = job.own_parms;
            if (job.parms == pdf_image::Parms::Predictor) parms = pdf_image::predictor_parms(job.image.image);
            job.obj.replaceStreamData(provider, QPDFObjectHandle::newName("/FlateDecode"), parms);
            break;
        }
        case StreamJob::Kind::Optimized:
            update_embedded_file_params(job.obj, job.file);
            job.obj.replaceStreamData(provider, dict.getKey("/Filter"), dict.getKey("/DecodeParms"));
            break;
        case StreamJob::Kind::Plain:
            if (job.to_flate) {
                job.obj.replaceStreamData(provider, QPDFObjectHandle::newName("/FlateDecode"), QPDFObjectHandle::newNull());
                break;
            }
            [[fallthrough]];
        case StreamJob::Kind::Predicted:
            job.obj.replaceStreamData(provider, dict.getKey("/Filter"), dict.getKey("/DecodeParms"));
            break;
    }
}

/**
 * @brief Compresses a batch of jobs on the thread pool, biggest first, then applies them in their order.
 */
void run_stream_jobs(std::vector<StreamJob>& jobs, const ProcessingOptions& options) {
    std::vector<std::size_t> order(jobs.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::ranges::stable_sort(order, std::ranges::greater{}, [&jobs](const std::size_t i) { return jobs[i].bytes(); });
    const auto compress = [&](const std::size_t i) { compress_stream(jobs[order[i]], options); };
    if (ThreadPool* pool = ThreadPool::current()) {
        pool->parallel_for(order.size(), compress);
    } else {
        for (std::size_t i = 0; i < order.size(); ++i) compress(i);
    }
    for (auto& job : jobs) apply_stream(job);
    jobs.clear();
}

/**
 * @brief Removes common metadata objects from a PDF.
 * @param pdf The QPDF instance to modify.
 */
void strip_metadata(QPDF& pdf) {
    QPDFObjectHandle trailer = pdf.getTrailer();
    if (trailer.isDictionary()) {
        if (trailer.hasKey("/Info")) trailer.removeKey("/Info");
        if (trailer.hasKey("/Metadata")) trailer.removeKey("/Metadata");
    }
    QPDFObjectHandle root = pdf.getRoot();
    if (root.isDictionary() && root.hasKey("/Metadata")) {
        root.removeKey("/Metadata");
    }
}

} // namespace

std::optional<ExtractedContent> PdfProcessor::prepare_extraction(const std::filesystem::path& input_path) {
    Logger::log(LogLevel::Debug, "Entering prepare_extraction for " + input_path.string(), get_name());

    ExtractedContent content;
    content.original_path = input_path;
    content.temp_dir = make_temp_dir_for(input_path, "pdf");

    LoggerStreamBuf warn_buf(LogLevel::Warning, "PdfProcessor");
    std::ostream warn_os(&warn_buf);
    LoggerStreamBuf err_buf(LogLevel::Error, "PdfProcessor");
    std::ostream err_os(&err_buf);

    QPDF pdf;
    auto qlogger = QPDFLogger::create();
    qlogger->setOutputStreams(&warn_os, &err_os);
    // setOutputStreams sends warnings to the error stream
    qlogger->setWarn(std::make_shared<Pl_OStream>("qpdf warnings", warn_os));
    pdf.setLogger(qlogger);
    pdf.processFile(input_path.string().c_str());

    auto objects = pdf.getAllObjects();
    const auto names = embedded_file_names(pdf);
    PdfState st;
    st.temp_dir = content.temp_dir;

    for (auto& obj : objects) {
        if (!obj.isStream()) continue;

        int obj_id = obj.getObjGen().getObj();
        auto& info = st.streams[obj_id];

        QPDFObjectHandle dict = obj.getDict();
        info.has_decode_parms = dict.isDictionary() && dict.hasKey("/DecodeParms");

        std::shared_ptr<Buffer> buf;
        std::vector<unsigned char> data;
        try {
            buf = obj.getStreamData(qpdf_dl_specialized);
            data.assign(buf->getBuffer(), buf->getBuffer() + buf->getSize());
            info.decodable = true;
        } catch (QPDFExc& e) {
            Logger::log(LogLevel::Debug,"Stream " + std::to_string(obj_id) + " is not decodable, falling back to raw: " + e.what(),
                                    get_name());
            buf = obj.getRawStreamData();
            data.assign(buf->getBuffer(), buf->getBuffer() + buf->getSize());
            info.decodable = false;
        }

        info.original_size = data.size();
        const std::string ext = file_extension(obj, info.decodable, names);
        if (ext.empty()) {
            // raw samples go through the png pipeline, which must keep the layout the dictionary declares
            if (!info.decodable || classify_single_filter(obj) == SingleFilterKind::None) continue;
            const auto layout = pdf_image::layout_of(obj, data.size());
            const auto png = layout ? pdf_image::to_png(*layout, data) : std::nullopt;
            if (!png) continue;
            std::filesystem::path png_file = content.temp_dir / ("object_" + std::to_string(obj_id) + ".png");
            if (!write_file(png_file, *png)) {
                cleanup_temp_dir(content.temp_dir, get_name());
                throw std::runtime_error("Can't write " + png_file.string());
            }
            info.file = png_file;
            info.image = layout;
            info.png_size = png->size();
            content.extracted_files.push_back(png_file);
            content.fixed_pixel_format.insert(png_file);
            continue;
        }

        std::filesystem::path out_file = content.temp_dir / ("object_" + std::to_string(obj_id) + ext);
        if (!write_file(out_file, data)) {
            cleanup_temp_dir(content.temp_dir, get_name());
            throw std::runtime_error("Can't write " + out_file.string());
        }

        info.file = out_file;
        content.extracted_files.push_back(out_file);
    }

    content.extras = std::make_any<PdfState>(std::move(st));
    content.format = ContainerFormat::Pdf;

    Logger::log(LogLevel::Debug, "Exiting prepare_extraction for " + input_path.string(), get_name());
    return content;
}

std::filesystem::path PdfProcessor::finalize_extraction(const ExtractedContent &content, const ProcessingOptions &options) {
    Logger::log(LogLevel::Debug, "Entering finalize_extraction for " + content.original_path.string(), get_name());

    enable_qpdf_zopfli_once(options);

    try {
        PdfState st;
        if (content.extras.has_value() && content.extras.type() == typeid(PdfState)) {
            st = std::any_cast<PdfState>(content.extras);
        } else {
            st.temp_dir = content.temp_dir;
        }

        LoggerStreamBuf warn_buf(LogLevel::Warning, "PdfProcessor");
        std::ostream warn_os(&warn_buf);
        LoggerStreamBuf err_buf(LogLevel::Error, "PdfProcessor");
        std::ostream err_os(&err_buf);

        QPDF pdf;
        auto qlogger = QPDFLogger::create();
        qlogger->setOutputStreams(&warn_os, &err_os);
        // setOutputStreams sends warnings to the error stream
        qlogger->setWarn(std::make_shared<Pl_OStream>("qpdf warnings", warn_os));
        pdf.setLogger(qlogger);

        pdf.processFile(content.original_path.string().c_str());

        auto objects = pdf.getAllObjects();

        // qpdf objects are read and changed on this thread only; the compression in between runs on the pool
        std::vector<StreamJob> jobs;
        std::size_t batch_bytes = 0;
        for (auto& obj : objects) {
            if (!obj.isStream()) continue;

            int obj_id = obj.getObjGen().getObj();
            if (!st.streams.contains(obj_id)) continue;
            auto& info = st.streams[obj_id];

            const QPDFObjectHandle dict = obj.getDict();
            const SingleFilterKind filter_kind = classify_single_filter(obj);
            bool is_flate = (filter_kind == SingleFilterKind::Flate);
            // LZWDecode/ASCII85Decode/ASCIIHexDecode all decode to plain,
            // uncompressed-or-text-encoded bytes with the same shape Flate
            // streams have; recompressing them means re-encoding as Flate
            // (there's no "LZW but smaller" -- Flate+zopfli is strictly the
            // better lossless choice), so the stream's /Filter has to change
            // along with its data, unlike the flate-stays-flate case below.
            bool is_convertible_to_flate =
                (filter_kind == SingleFilterKind::Lzw ||
                 filter_kind == SingleFilterKind::Ascii85 ||
                 filter_kind == SingleFilterKind::AsciiHex);
            bool has_decode_parms = dict.isDictionary() && dict.hasKey("/DecodeParms");

            StreamJob job{.obj=obj, .obj_id=obj_id};
            if (info.image) {
                try {
                    job.kind = StreamJob::Kind::Image;
                    job.image = pdf_image::read_image(obj, *info.image, info.file, info.png_size, job.own_parms);
                } catch (const std::exception& e) {
                    Logger::log(LogLevel::Debug, "Image obj " + std::to_string(obj_id) + " left as is: " + e.what(), get_name());
                    continue;
                }
            } else {
                std::error_code ec;
                uintmax_t disk_size = info.file.empty() ? 0 : std::filesystem::file_size(info.file, ec);
                bool file_was_optimized = (!ec && disk_size > 0 && disk_size < info.original_size);

                // read externally optimized file if it exists (e.g. jpg optimized by Phase 2)
                if (file_was_optimized) {
                    std::ifstream ifs(info.file, std::ios::binary | std::ios::ate);
                    if (!ifs) continue;
                    auto sz = ifs.tellg(); ifs.seekg(0);
                    job.data.resize(sz);
                    ifs.read(reinterpret_cast<char*>(job.data.data()), sz);
                    job.kind = StreamJob::Kind::Optimized;
                    job.deflate = is_flate;
                    job.file = info.file;
                }
                // if no external file, but it's a plain flate/LZW/ASCII85/ASCIIHex stream
                // with no Predictor complications, optimize with zopfli
                else if ((is_flate || is_convertible_to_flate) && !has_decode_parms && info.decodable) {
                    try {
                        const std::shared_ptr<Buffer> buf = obj.getStreamData(qpdf_dl_specialized);
                        job.data.assign(buf->getBuffer(), buf->getBuffer() + buf->getSize());
                        job.raw_size = obj.getRawStreamData()->getSize();
                    } catch (const std::exception&) {
                        Logger::log(LogLevel::Debug, "zopfli skipped on obj " + std::to_string(obj_id), get_name());
                        continue;
                    }
                    job.kind = StreamJob::Kind::Plain;
                    job.to_flate = is_convertible_to_flate;
                }
                // with /DecodeParms the inflated bytes are still the predictor's output, and deflating them
                // again as they are keeps /DecodeParms valid
                else if (is_flate && has_decode_parms && info.decodable) {
                    try {
                        const std::shared_ptr<Buffer> raw = obj.getRawStreamData();
                        job.data.assign(raw->getBuffer(), raw->getBuffer() + raw->getSize());
                        job.raw_size = raw->getSize();
                    } catch (const std::exception&) {
                        Logger::log(LogLevel::Debug, "zopfli skipped on obj " + std::to_string(obj_id), get_name());
                        continue;
                    }
                    job.kind = StreamJob::Kind::Predicted;
                } else {
                    continue;
                }
            }

            batch_bytes += job.bytes();
            jobs.push_back(std::move(job));
            if (batch_bytes >= kStreamBatchBytes) {
                run_stream_jobs(jobs, options);
                batch_bytes = 0;
            }
        }
        run_stream_jobs(jobs, options);

        if (!options.preserve_metadata) {
            strip_metadata(pdf);
        }

        // Always write to a new temporary file
        auto tmp_path = content.original_path;
        tmp_path += ".refinalized.pdf";

        QPDFWriter writer(pdf, tmp_path.string().c_str());
        // linearizing a file that wasn't would add hint tables to it, and it makes qpdf write everything twice
        writer.setLinearization(pdf.isLinearized());
        writer.setStaticID(true);
        writer.setDeterministicID(true);
        writer.setObjectStreamMode(qpdf_o_generate);
        writer.setStreamDataMode(qpdf_s_compress);
        writer.write();

        cleanup_temp_dir(st.temp_dir, get_name());

        Logger::log(LogLevel::Debug, "Exiting finalize_extraction for " + tmp_path.string(), get_name());

        return tmp_path;
    } catch (const std::exception& e) {
        Logger::log(LogLevel::Error, std::string("Exception during pdf finalize: ") + e.what(), get_name());
        throw;
    }
}

/**
 * @brief Extracts the decoded content of all streams from a PDF file.
 * This is used for checksum verification. Decoded (not raw/still-compressed)
 * content is compared, since recompression legitimately changes the raw
 * bytes of a stream (e.g. re-flating with zopfli) without changing its content.
 * Streams are collected into a vector (not keyed by object number/generation),
 * since operations like linearization freely renumber objects between the
 * original and finalized file even when no stream content actually changed.
 * @param path The path to the PDF file.
 * @param streams A vector to be populated with each stream's decoded data.
 * @return True on success, false if the PDF could not be processed.
 */
static bool get_all_decoded_streams(const std::filesystem::path& path,
                                    std::vector<std::vector<uint8_t>>& streams)
{
    try {
        QPDF pdf;
        const auto qlogger = QPDFLogger::create();
        std::ostream warn_os(nullptr);
        std::ostream err_os(nullptr);
        qlogger->setOutputStreams(&warn_os, &err_os);
        pdf.setLogger(qlogger);
        pdf.processFile(path.string().c_str());

        auto objects = pdf.getAllObjects();
        for (auto& obj : objects) {
            if (!obj.isStream()) continue;

            // /ObjStm and /XRef streams are QPDFWriter's own low-level serialization
            // plumbing (object streams, cross-reference streams), not PDF content;
            // their presence/count/encoding legitimately differs between writer runs
            // (e.g. after linearization) even when no actual content changed
            const QPDFObjectHandle dict = obj.getDict();
            if (dict.isDictionary() && dict.hasKey("/Type") && dict.getKey("/Type").isName()) {
                const std::string type = dict.getKey("/Type").getName();
                if (type == "/ObjStm" || type == "/XRef") continue;
            }

            std::shared_ptr<Buffer> buf;
            try {
                buf = obj.getStreamData(qpdf_dl_specialized);
            } catch (QPDFExc&) {
                buf = obj.getRawStreamData();
            }
            streams.emplace_back(buf->getBuffer(), buf->getBuffer() + buf->getSize());
        }
        std::sort(streams.begin(), streams.end());
        return true;
    } catch (const std::exception& e) {
        Logger::log(LogLevel::Warning, "Failed to read pdf streams: " + std::string(e.what()), "PdfProcessor");
        return false;
    }
}
    bool PdfProcessor::raw_equal(const std::filesystem::path& a,
                             const std::filesystem::path& b) const {
    std::vector<std::vector<uint8_t>> streamsA, streamsB;

    const bool okA = get_all_decoded_streams(a, streamsA);
    const bool okB = get_all_decoded_streams(b, streamsB);

    if (!okA || !okB) {
        return false; // failed to read one or both
    }

    // every stream a's own content had must still be present in b, but b is
    // allowed extra streams a didn't have: QPDFWriter's own linearization
    // (always on) synthesizes a hint stream that has neither /ObjStm nor
    // /XRef as its /Type -- in fact no /Type at all -- so it isn't caught by
    // get_all_decoded_streams's exclusion above, and requiring exact
    // equality made this reject every successfully linearized file. Both
    // sides are already sorted, so this is a multiset-subset check: it still
    // catches real content loss (a stream vanishing) or corruption (a
    // stream's bytes changing), just not qpdf's own added bookkeeping.
    return std::includes(streamsB.begin(), streamsB.end(), streamsA.begin(), streamsA.end());
}
std::string PdfProcessor::get_raw_checksum(const std::filesystem::path&) const {
    // TODO: implement checksum of raw PDF data
    return "";
}

bool PdfProcessor::is_signed(const std::filesystem::path& file_path) const {
    // signature dictionaries can't sit in object streams, so their /ByteRange key is in the raw bytes
    return file_contains(file_path, "/ByteRange");
}

} // namespace chisel