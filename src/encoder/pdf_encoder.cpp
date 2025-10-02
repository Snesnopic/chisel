//
// Created by Giuseppe Francione on 28/09/25.
//

#include "pdf_encoder.hpp"
#include "../utils/logger.hpp"
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFWriter.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/Buffer.hh>
#include <filesystem>
#include <vector>
#include <string>
#include <stdexcept>
#include <memory>
#include <qpdf/QPDFLogger.hh>
#include "zopfli.h"
#include "zlib_container.h"

namespace fs = std::filesystem;
// helper: custom streambuf to redirect qpdf messages into our logger
struct LoggerStreamBuf : std::stringbuf {
    LogLevel level;
    std::string module;

    LoggerStreamBuf(const LogLevel lvl, const char* mod): level(lvl), module(mod){}

    // log immediately when the buffer is flushed
    int sync() override {
        std::string s = str();
        if (!s.empty()) {
            Logger::log(level, s, module);
            str(""); // clear buffer after logging
        }
        return 0;
    }

    ~LoggerStreamBuf() override {
        // flush any remaining content
        LoggerStreamBuf::sync();
    }
};

// helper: recompress data with zopfli (zlib container)
static std::vector<unsigned char> recompress_with_zopfli(const std::vector<unsigned char>& input)
{
    ZopfliOptions opts;
    ZopfliInitOptions(&opts);
    opts.numiterations = 15; // higher is better but slower
    opts.blocksplitting = 1;

    unsigned char* out_data = nullptr;
    size_t out_size = 0;
    ZopfliZlibCompress(&opts, input.data(), input.size(), &out_data, &out_size);

    std::vector<unsigned char> result(out_data, out_data + out_size);
    free(out_data);
    return result;
}

// helper: check whether a stream uses only /FlateDecode (single filter)
static bool stream_is_single_flate(QPDFObjectHandle const& stream)
{
    if (!stream.isStream()) return false;

    const QPDFObjectHandle dict = stream.getDict();
    if (!dict.isDictionary()) return false;

    const QPDFObjectHandle filter = dict.getKey("/Filter");
    if (filter.isName()) {
        return (filter.getName() == "/FlateDecode");
    }
    if (filter.isArray()) {
        if (filter.getArrayNItems() != 1) {
            return false;
        }
        const QPDFObjectHandle item = filter.getArrayItem(0);
        return (item.isName() && item.getName() == "/FlateDecode");
    }
    return false;
}

// helper: strip metadata if preserve_metadata_ is false
static void strip_metadata(QPDF& pdf)
{
    QPDFObjectHandle trailer = pdf.getTrailer();
    if (trailer.isDictionary()) {
        if (trailer.hasKey("/Info")) {
            trailer.removeKey("/Info");
        }
        if (trailer.hasKey("/Metadata")) {
            trailer.removeKey("/Metadata");
        }
    }

    QPDFObjectHandle root = pdf.getRoot();
    if (root.isDictionary() && root.hasKey("/Metadata")) {
        root.removeKey("/Metadata");
    }
}

PdfEncoder::PdfEncoder(const bool preserve_metadata) {
    preserve_metadata_ = preserve_metadata;
}

bool PdfEncoder::recompress(const fs::path& input,
                            const fs::path& output)
{
    Logger::log(LogLevel::INFO, "Starting PDF optimization: " + input.string(), "PdfEncoder");

    try {
        QPDF pdf;

        // redirect qpdf warnings and errors into logger
        LoggerStreamBuf warn_buf{LogLevel::WARNING, "PdfEncoder"};
        LoggerStreamBuf err_buf{LogLevel::ERROR, "PdfEncoder"};
        std::ostream warn_os(&warn_buf);
        std::ostream err_os(&err_buf);
        auto qlogger = QPDFLogger::create();
        qlogger->setOutputStreams(&warn_os, &err_os);
        pdf.setLogger(qlogger);

        pdf.processFile(input.string().c_str());

        if (!preserve_metadata_) {
            Logger::log(LogLevel::INFO, "Stripping metadata (Info/XMP)", "PdfEncoder");
            strip_metadata(pdf);
        }

        // iterate all objects and recompress flate streams
        std::vector<QPDFObjectHandle> objects = pdf.getAllObjects();
        for (auto& obj : objects) {
            if (!obj.isStream())
                continue;

            const QPDFObjectHandle dict = obj.getDict();
            if (dict.isDictionary() && dict.hasKey("/DecodeParms")) {
                Logger::log(LogLevel::DEBUG, "Skipping stream with DecodeParms (predictor present)", "PdfEncoder");
                continue;
            }

            if (!stream_is_single_flate(obj)) {
                Logger::log(LogLevel::DEBUG, "Skipping stream with non-single Flate filter chain", "PdfEncoder");
                continue;
            }

            const std::shared_ptr<Buffer> buf = obj.getStreamData(qpdf_dl_specialized);
            const std::vector<unsigned char> decoded(buf->getBuffer(), buf->getBuffer() + buf->getSize());

            std::vector<unsigned char> recompressed = recompress_with_zopfli(decoded);

            obj.replaceStreamData(
                std::string(reinterpret_cast<const char*>(recompressed.data()), recompressed.size()),
                QPDFObjectHandle::newName("/FlateDecode"),
                QPDFObjectHandle::newNull()
            );
        }

        QPDFWriter writer(pdf, output.string().c_str());
        writer.setLinearization(true);
        writer.setStaticID(true);
        writer.setDeterministicID(true);
        writer.write();

        Logger::log(LogLevel::INFO, "PDF optimization finished: " + output.string(), "PdfEncoder");
        return true;
    }
    catch (const std::exception& e) {
        Logger::log(LogLevel::ERROR, std::string("Exception during PDF optimization: ") + e.what(), "PdfEncoder");
        return false;
    }
}