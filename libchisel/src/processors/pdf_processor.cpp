//
// Created by Giuseppe Francione on 19/10/25.
//

#include "../../include/pdf_processor.hpp"
#include "../../include/file_type.hpp"
#include "../../include/logger.hpp"
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFWriter.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/Buffer.hh>
#include <qpdf/QPDFLogger.hh>
#include <fstream>
#include <memory>
#include <string>
#include <vector>
#include <random>
#include <chrono>
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
        bool supportsCompression() {
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
                    std::string fname = filter.getName();
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
 * @brief Checks if a PDF stream is compressed only with FlateDecode.
 * @param stream The QPDFObjectHandle for the stream.
 * @return True if the stream uses a single /FlateDecode filter, false otherwise.
 */
bool stream_is_single_flate(QPDFObjectHandle const& stream) {
    if (!stream.isStream()) return false;
    const QPDFObjectHandle dict = stream.getDict();
    if (!dict.isDictionary()) return false;
    const QPDFObjectHandle filter = dict.getKey("/Filter");
    if (filter.isName()) return (filter.getName() == "/FlateDecode");
    if (filter.isArray() && filter.getArrayNItems() == 1) {
        const QPDFObjectHandle item = filter.getArrayItem(0);
        return (item.isName() && item.getName() == "/FlateDecode");
    }
    return false;
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
    content.temp_dir = make_temp_dir_for(input_path);

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
            if (st.streams.find(obj_id) == st.streams.end()) continue;
            auto& info = st.streams[obj_id];

            const QPDFObjectHandle dict = obj.getDict();
            bool is_flate = stream_is_single_flate(obj);
            bool has_decode_parms = dict.isDictionary() && dict.hasKey("/DecodeParms");

            std::error_code ec;
            uintmax_t disk_size = info.file.empty() ? 0 : std::filesystem::file_size(info.file, ec);

            bool file_was_optimized = (!ec && disk_size > 0 && disk_size < info.original_size);
            std::vector<unsigned char> raw_data_to_inject;

            bool replace_stream = false;

            // read externally optimized file if it exists (e.g. jpg optimized by Phase 2)
            if (file_was_optimized) {
                std::ifstream ifs(info.file, std::ios::binary | std::ios::ate);
                if (ifs) {
                    auto sz = ifs.tellg(); ifs.seekg(0);
                    raw_data_to_inject.resize(sz);
                    ifs.read(reinterpret_cast<char*>(raw_data_to_inject.data()), sz);
                    replace_stream = true;
                }
            }
            // if no external file, but it's an internal flate stream, optimize with zopfli
            else if (is_flate && !has_decode_parms && info.decodable) {
                try {
                    const std::shared_ptr<Buffer> buf = obj.getStreamData(qpdf_dl_specialized);
                    std::vector<unsigned char> decoded(buf->getBuffer(), buf->getBuffer() + buf->getSize());
                    auto recompressed = ZopfliCompressor::compress(decoded, options.iterations, ZopfliFormat::ZLIB);

                    if (recompressed.size() < obj.getRawStreamData()->getSize()) {
                        raw_data_to_inject = std::move(recompressed);
                        replace_stream = true;
                    }
                } catch (const std::exception& e) {
                    Logger::log(LogLevel::Debug, "zopfli skipped on obj " + std::to_string(obj_id), get_name());
                }
            }

            // inject the raw data keeping the original dictionary filters intact
            if (replace_stream) {
                auto provider = std::make_shared<raw_stream_provider>(std::move(raw_data_to_inject));
                obj.replaceStreamData(provider, dict.getKey("/Filter"), dict.getKey("/DecodeParms"));
            }
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

        cleanup_temp_dir(st.temp_dir);

        Logger::log(LogLevel::Debug, "Exiting finalize_extraction for " + tmp_path.string(), get_name());

        return tmp_path;
    } catch (const std::exception& e) {
        Logger::log(LogLevel::Error, std::string("Exception during pdf finalize: ") + e.what(), get_name());
        throw;
    }
}

/**
 * @brief Extracts all raw (unfiltered) streams from a PDF file.
 * This is used for checksum verification.
 * @param path The path to the PDF file.
 * @param streams A map to be populated with object numbers and their raw stream data.
 * @return True on success, false if the PDF could not be processed.
 */
static bool get_all_raw_streams(const std::filesystem::path& path,
                                    std::map<int, std::vector<uint8_t>>& streams)
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
            if (obj.isStream()) {
                const std::shared_ptr<Buffer> buf = obj.getRawStreamData();
                streams[obj.getObjGen().getObj()] =
                    std::vector<uint8_t>(buf->getBuffer(), buf->getBuffer() + buf->getSize());
            }
        }
        return true;
    } catch (const std::exception& e) {
        Logger::log(LogLevel::Warning, "Failed to read pdf streams: " + std::string(e.what()), "PdfProcessor");
        return false;
    }
}
    bool PdfProcessor::raw_equal(const std::filesystem::path& a,
                             const std::filesystem::path& b) const {
    std::map<int, std::vector<uint8_t>> streamsA, streamsB;

    bool okA = get_all_raw_streams(a, streamsA);
    bool okB = get_all_raw_streams(b, streamsB);

    if (!okA || !okB) {
        return false; // failed to read one or both
    }

    if (streamsA.size() != streamsB.size()) {
        return false; // different number of streams
    }

    // compare map contents (obj-id -> raw_stream_bytes)
    return streamsA == streamsB;
}
std::string PdfProcessor::get_raw_checksum(const std::filesystem::path&) const {
    // TODO: implement checksum of raw PDF data
    return "";
}

std::filesystem::path PdfProcessor::make_temp_dir_for(const std::filesystem::path& input) {
    const auto base_tmp = std::filesystem::temp_directory_path() / "chisel-pdf";
    std::error_code ec;
    std::filesystem::create_directories(base_tmp, ec);

    auto now = std::chrono::system_clock::now().time_since_epoch();
    auto ts = std::chrono::duration_cast<std::chrono::seconds>(now).count();

    std::mt19937_64 rng{static_cast<unsigned long long>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count())};
    const unsigned long long r = rng();

    const std::string stem = input.stem().string();
    const std::string dir_name = stem + "-" + std::to_string(ts) + "-" + std::to_string(r & 0xFFFFULL);
    auto dir = base_tmp / dir_name;

    std::filesystem::create_directories(dir, ec);
    return dir;
}

void PdfProcessor::cleanup_temp_dir(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    if (ec) {
        Logger::log(LogLevel::Warning, "Failed to cleanup temp dir: " + dir.string(), "PdfProcessor");
    }
}

} // namespace chisel