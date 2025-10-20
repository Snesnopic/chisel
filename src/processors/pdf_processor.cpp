//
// Created by Giuseppe Francione on 19/10/25.
//

#include "pdf_processor.hpp"
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
#include "../utils/file_type.hpp"

namespace {

// helper: custom streambuf to redirect qpdf messages into our logger
struct LoggerStreamBuf final : std::stringbuf {
    LogLevel level;
    std::string module;
    LoggerStreamBuf(const LogLevel lvl, const char* mod) : level(lvl), module(mod) {}
    int sync() override {
        std::string s = str();
        if (!s.empty()) {
            Logger::log(level, s, module);
            str("");
        }
        return 0;
    }
    ~LoggerStreamBuf() override { LoggerStreamBuf::sync(); }
};

// helper: guess extension from pdf stream dictionary and data
static std::string guess_extension(QPDFObjectHandle const& stream,
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

// helper: recompress data with zopfli
static std::vector<unsigned char> recompress_with_zopfli(const std::vector<unsigned char>& input) {
    ZopfliOptions opts;
    ZopfliInitOptions(&opts);
    opts.numiterations = 15;
    opts.blocksplitting = 1;
    unsigned char* out_data = nullptr;
    size_t out_size = 0;
    ZopfliZlibCompress(&opts, input.data(), input.size(), &out_data, &out_size);
    std::vector<unsigned char> result(out_data, out_data + out_size);
    free(out_data);
    return result;
}

// helper: check whether a stream uses only /FlateDecode
static bool stream_is_single_flate(QPDFObjectHandle const& stream) {
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

// helper: strip metadata
static void strip_metadata(QPDF& pdf) {
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

namespace chisel {

std::optional<ExtractedContent> PdfProcessor::prepare_extraction(const std::filesystem::path& input_path) {
    Logger::log(LogLevel::Info, "Preparing PDF container: " + input_path.string(), "pdf_processor");

    ExtractedContent content;
    content.original_path = input_path;
    content.temp_dir = make_temp_dir_for(input_path);

    QPDF pdf;
    auto qlogger = QPDFLogger::create();
    std::ostream warn_os(nullptr);
    std::ostream err_os(nullptr);
    qlogger->setOutputStreams(&warn_os, &err_os);
    pdf.setLogger(qlogger);
    pdf.processFile(input_path.string().c_str());

    auto objects = pdf.getAllObjects();
    PdfState st;
    st.streams.resize(objects.size());
    st.temp_dir = content.temp_dir;

    for (size_t i = 0; i < objects.size(); ++i) {
        auto& obj = objects[i];
        if (!obj.isStream()) continue;
        auto& info = st.streams[i];
        QPDFObjectHandle dict = obj.getDict();
        info.has_decode_parms = dict.isDictionary() && dict.hasKey("/DecodeParms");

        std::shared_ptr<Buffer> buf;
        std::vector<unsigned char> data;
        try {
            buf = obj.getStreamData(qpdf_dl_specialized);
            data.assign(buf->getBuffer(), buf->getBuffer() + buf->getSize());
            info.decodable = true;
        } catch (QPDFExc&) {
            buf = obj.getRawStreamData();
            data.assign(buf->getBuffer(), buf->getBuffer() + buf->getSize());
            info.decodable = false;
        }

        std::string ext = guess_extension(obj, data);
        std::filesystem::path out_file = content.temp_dir / ("object_" + std::to_string(i) + ext);

        std::ofstream ofs(out_file, std::ios::binary);
        ofs.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        ofs.close();

        info.file = out_file;
        content.extracted_files.push_back(out_file);
    }

    state_[content.original_path] = std::move(st);
    content.format = ContainerFormat::Pdf;
    return content;
}

void PdfProcessor::finalize_extraction(const ExtractedContent& content,
                                       ContainerFormat /*target_format*/) {
    Logger::log(LogLevel::Info, "Finalizing PDF container: " + content.original_path.string(), "pdf_processor");

    try {
        PdfState st;
        if (auto it = state_.find(content.original_path); it != state_.end()) {
            st = it->second;
        } else {
            st.temp_dir = content.temp_dir;
        }

        QPDF pdf;
        auto qlogger = QPDFLogger::create();
        std::ostream warn_os(nullptr);
        std::ostream err_os(nullptr);
        qlogger->setOutputStreams(&warn_os, &err_os);
        pdf.setLogger(qlogger);

        pdf.processFile(content.original_path.string().c_str());

        auto objects = pdf.getAllObjects();
        if (st.streams.size() < objects.size()) {
            st.streams.resize(objects.size());
        }

        for (size_t i = 0; i < objects.size(); ++i) {
            auto& obj = objects[i];
            if (!obj.isStream()) continue;

            auto& info = st.streams[i];
            if (!info.decodable) continue;

            const QPDFObjectHandle dict = obj.getDict();
            if (dict.isDictionary() && dict.hasKey("/DecodeParms")) continue;
            if (!stream_is_single_flate(obj)) continue;

            std::vector<unsigned char> decoded;
            if (!info.file.empty() && std::filesystem::exists(info.file)) {
                std::ifstream ifs(info.file, std::ios::binary);
                decoded.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
                ifs.close();
            } else {
                try {
                    const std::shared_ptr<Buffer> buf = obj.getStreamData(qpdf_dl_specialized);
                    decoded.assign(buf->getBuffer(), buf->getBuffer() + buf->getSize());
                } catch (QPDFExc& e) {
                    Logger::log(LogLevel::Debug,
                                "Skipping stream " + std::to_string(i) + " (not decodable now): " + std::string(e.what()),
                                "pdf_processor");
                    continue;
                }
            }

            std::vector<unsigned char> recompressed = recompress_with_zopfli(decoded);

            obj.replaceStreamData(
                std::string(reinterpret_cast<const char*>(recompressed.data()), recompressed.size()),
                QPDFObjectHandle::newName("/FlateDecode"),
                QPDFObjectHandle::newNull()
            );
        }

        auto tmp_path = content.original_path;
        tmp_path += ".tmp";

        QPDFWriter writer(pdf, tmp_path.string().c_str());
        writer.setLinearization(true);
        writer.setStaticID(true);
        writer.setDeterministicID(true);
        writer.write();

        auto orig_size = std::filesystem::file_size(content.original_path);
        auto new_size = std::filesystem::file_size(tmp_path);

        if (new_size < orig_size) {
            std::filesystem::rename(tmp_path, content.original_path);
        } else {
            std::filesystem::remove(tmp_path);
        }

        cleanup_temp_dir(st.temp_dir);
        state_.erase(content.original_path);

        Logger::log(LogLevel::Info, "PDF container finalized: " + content.original_path.string(), "pdf_processor");
    } catch (const std::exception& e) {
        Logger::log(LogLevel::Error, std::string("Exception during PDF finalize: ") + e.what(), "pdf_processor");
        throw;
    }
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
        Logger::log(LogLevel::Warning, "Failed to cleanup temp dir: " + dir.string(), "pdf_processor");
    }
}

} // namespace chisel