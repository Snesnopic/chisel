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
#include <qpdf/Pl_Flate.hh>
#include <fstream>
#include <sstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdlib>
#include "zlib_container.h"
#include "zopfli.h"
#include "zopfli_compressor.hpp"

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
 * @brief Guesses a file extension for a PDF stream based on its dictionary and content.
 * @param stream The QPDFObjectHandle for the stream.
 * @param data The raw (decoded) stream data.
 * @return A string representing the guessed file extension (e.g., ".jpg", ".png").
 */
std::string guess_extension(QPDFObjectHandle const& stream,
                                   const std::vector<unsigned char>& data) {
    if (!stream.isStream()) return ".bin";
    const QPDFObjectHandle dict = stream.getDict();
    if (dict.hasKey("/Subtype") && dict.getKey("/Subtype").isName()) {
        const std::string subtype = dict.getKey("/Subtype").getName();
        if (subtype == "/Image") {
            if (dict.hasKey("/Filter")) {
                const auto filter = dict.getKey("/Filter");
                if (filter.isName()) {
                    const std::string fname = filter.getName();
                    if (fname == "/DCTDecode") return ".jpg";
                    if (fname == "/JPXDecode") return ".jp2";
                    if (fname == "/FlateDecode") {
                        if (data.size() >= 8 &&
                            data[0] == 0x89 && data[1] == 0x50 &&
                            data[2] == 0x4E && data[3] == 0x47) {
                            return ".png";
                        }
                        return ".raw";
                    }
                }
            }
        }
        if (subtype == "/Form") return ".form";
    }
    if (dict.hasKey("/FontFile2")) return ".ttf";
    if (dict.hasKey("/FontFile3")) {
        if (dict.hasKey("/Subtype") && dict.getKey("/Subtype").isName() &&
            dict.getKey("/Subtype").getName() == "/Type1C") {
            return ".otf";
        }
        return ".cff";
    }
    if (dict.hasKey("/Type") && dict.getKey("/Type").isName() &&
        dict.getKey("/Type").getName() == "/Metadata") {
        return ".xml";
    }
    if (data.size() >= 4) {
        if (data[0] == 0xFF && data[1] == 0xD8) return ".jpg";
        if (data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E && data[3] == 0x47) return ".png";
        if (data[0] == 0x25 && data[1] == 0x50 && data[2] == 0x44 && data[3] == 0x46) return ".pdf";
        if (data[0] == 0x4F && data[1] == 0x54 && data[2] == 0x54 && data[3] == 0x4F) return ".otf";
    }
    return ".bin";
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
    pdf.setLogger(qlogger);
    pdf.processFile(input_path.string().c_str());

    auto objects = pdf.getAllObjects();
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

        std::string ext = guess_extension(obj, data);
        std::filesystem::path out_file = content.temp_dir / ("object_" + std::to_string(obj_id) + ext);

        std::ofstream ofs(out_file, std::ios::binary);
        ofs.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        ofs.close();

        info.file = out_file;
        info.original_size = data.size();
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
        pdf.setLogger(qlogger);

        pdf.processFile(content.original_path.string().c_str());

        auto objects = pdf.getAllObjects();

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

            std::error_code ec;
            uintmax_t disk_size = info.file.empty() ? 0 : std::filesystem::file_size(info.file, ec);

            bool file_was_optimized = (!ec && disk_size > 0 && disk_size < info.original_size);
            std::vector<unsigned char> raw_data_to_inject;

            bool replace_stream = false;
            // only set when the injected data needs a different /Filter than
            // the stream originally declared (the Lzw/Ascii85/AsciiHex ->
            // Flate conversion below); left unset (empty) otherwise, meaning
            // "keep the dict's own /Filter and /DecodeParms as they are"
            QPDFObjectHandle new_filter;

            // read externally optimized file if it exists (e.g. jpg optimized by Phase 2)
            if (file_was_optimized) {
                std::ifstream ifs(info.file, std::ios::binary | std::ios::ate);
                if (ifs) {
                    auto sz = ifs.tellg(); ifs.seekg(0);
                    raw_data_to_inject.resize(sz);
                    ifs.read(reinterpret_cast<char*>(raw_data_to_inject.data()), sz);

                    // the extracted file holds *decoded* content (e.g. a full PNG file
                    // guessed from a FlateDecode Image stream); the original filter is
                    // being kept on the dict, so the bytes must be re-compressed to stay
                    // valid under it. DCTDecode/JPXDecode extractions are already in their
                    // final on-disk form and need no re-wrapping.
                    if (is_flate) {
                        try {
                            raw_data_to_inject = ZopfliCompressor::compress(
                                raw_data_to_inject, pick_iterations(raw_data_to_inject.size(), options),
                                ZopfliFormat::ZLIB);
                        } catch (const std::exception& e) {
                            Logger::log(LogLevel::Warning,
                                        "Failed to re-compress optimized stream for obj " +
                                        std::to_string(obj_id) + ", skipping injection: " + e.what(),
                                        get_name());
                            raw_data_to_inject.clear();
                        }
                    }

                    replace_stream = !raw_data_to_inject.empty();
                }
            }
            // if no external file, but it's a plain flate/LZW/ASCII85/ASCIIHex stream
            // with no Predictor complications, optimize with zopfli
            else if ((is_flate || is_convertible_to_flate) && !has_decode_parms && info.decodable) {
                try {
                    const std::shared_ptr<Buffer> buf = obj.getStreamData(qpdf_dl_specialized);
                    std::vector<unsigned char> decoded(buf->getBuffer(), buf->getBuffer() + buf->getSize());
                    auto recompressed = ZopfliCompressor::compress(
                        decoded, pick_iterations(decoded.size(), options), ZopfliFormat::ZLIB);

                    if (recompressed.size() < obj.getRawStreamData()->getSize()) {
                        raw_data_to_inject = std::move(recompressed);
                        replace_stream = true;
                        if (is_convertible_to_flate) {
                            new_filter = QPDFObjectHandle::newName("/FlateDecode");
                        }
                    }
                } catch (const std::exception& e) {
                    Logger::log(LogLevel::Debug, "zopfli skipped on obj " + std::to_string(obj_id), get_name());
                }
            }

            // inject the raw data, keeping the original dictionary filters
            // intact unless new_filter says otherwise
            if (replace_stream) {
                auto provider = std::make_shared<raw_stream_provider>(std::move(raw_data_to_inject));
                if (new_filter.isInitialized()) {
                    obj.replaceStreamData(provider, new_filter, QPDFObjectHandle::newNull());
                } else {
                    obj.replaceStreamData(provider, dict.getKey("/Filter"), dict.getKey("/DecodeParms"));
                }
            }
        }

        if (!options.preserve_metadata) {
            strip_metadata(pdf);
        }

        // Always write to a new temporary file
        auto tmp_path = content.original_path;
        tmp_path += ".refinalized.pdf";

        QPDFWriter writer(pdf, tmp_path.string().c_str());
        writer.setLinearization(true);
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

} // namespace chisel