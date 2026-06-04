//
// Created by Giuseppe Francione on 04/06/26.
//

#include "../../include/jp2_processor.hpp"
#include "../../include/logger.hpp"
#include <openjpeg.h>
#include <filesystem>
#include <stdexcept>
#include <iostream>

namespace chisel {

// openjpeg logging callbacks
static void error_callback(const char* msg, void* /*client_data*/) {
    Logger::log(LogLevel::Error, "OpenJPEG Error: " + std::string(msg), "Jp2Processor");
}

static void warning_callback(const char* msg, void* /*client_data*/) {
    Logger::log(LogLevel::Warning, "OpenJPEG Warning: " + std::string(msg), "Jp2Processor");
}

static void info_callback(const char* msg, void* /*client_data*/) {
    Logger::log(LogLevel::Debug, "OpenJPEG Info: " + std::string(msg), "Jp2Processor");
}

void Jp2Processor::recompress(const std::filesystem::path& input_path,
                               const std::filesystem::path& output_path,
                               const ProcessingOptions &/*options*/) {
    Logger::log(LogLevel::Debug, "Entering recompress for " + input_path.string(), get_name());

    // determine codec format based on extension
    OPJ_CODEC_FORMAT format = OPJ_CODEC_JP2;
    std::string ext = input_path.extension().string();
    std::ranges::transform(ext, ext.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    
    if (ext == ".j2k" || ext == ".j2c") {
        format = OPJ_CODEC_J2K;
    }

    // --- DECODER SETUP ---
    opj_dparameters_t dparam;
    opj_set_default_decoder_parameters(&dparam);

    opj_stream_t* in_stream = opj_stream_create_default_file_stream(input_path.string().c_str(), OPJ_TRUE);
    if (!in_stream) {
        throw std::runtime_error("Jp2Processor: failed to open input file stream");
    }

    opj_codec_t* decoder = opj_create_decompress(format);
    opj_set_info_handler(decoder, info_callback, nullptr);
    opj_set_warning_handler(decoder, warning_callback, nullptr);
    opj_set_error_handler(decoder, error_callback, nullptr);

    if (!opj_setup_decoder(decoder, &dparam)) {
        opj_stream_destroy(in_stream);
        opj_destroy_codec(decoder);
        throw std::runtime_error("Jp2Processor: failed to setup decoder");
    }

    opj_image_t* image = nullptr;
    if (!opj_read_header(in_stream, decoder, &image)) {
        opj_stream_destroy(in_stream);
        opj_destroy_codec(decoder);
        throw std::runtime_error("Jp2Processor: failed to read header");
    }

    if (!opj_decode(decoder, in_stream, image)) {
        opj_image_destroy(image);
        opj_stream_destroy(in_stream);
        opj_destroy_codec(decoder);
        throw std::runtime_error("Jp2Processor: failed to decode image");
    }
    
    opj_end_decompress(decoder, in_stream);
    opj_stream_destroy(in_stream);
    opj_destroy_codec(decoder);

    // --- ENCODER SETUP ---
    opj_cparameters_t cparam;
    opj_set_default_encoder_parameters(&cparam);

    // lossless configuration
    cparam.tcp_numlayers = 1;
    cparam.tcp_rates[0] = 0; 
    cparam.cp_disto_alloc = 1;
    cparam.irreversible = 0; // Use 5/3 wavelet transform

    opj_codec_t* encoder = opj_create_compress(format);
    opj_set_info_handler(encoder, info_callback, nullptr);
    opj_set_warning_handler(encoder, warning_callback, nullptr);
    opj_set_error_handler(encoder, error_callback, nullptr);

    if (!opj_setup_encoder(encoder, &cparam, image)) {
        opj_image_destroy(image);
        opj_destroy_codec(encoder);
        throw std::runtime_error("Jp2Processor: failed to setup encoder");
    }

    opj_stream_t* out_stream = opj_stream_create_default_file_stream(output_path.string().c_str(), OPJ_FALSE);
    if (!out_stream) {
        opj_image_destroy(image);
        opj_destroy_codec(encoder);
        throw std::runtime_error("Jp2Processor: failed to open output file stream");
    }

    // execute compression
    bool success = opj_start_compress(encoder, image, out_stream) &&
                   opj_encode(encoder, out_stream) &&
                   opj_end_compress(encoder, out_stream);

    // cleanup
    opj_stream_destroy(out_stream);
    opj_destroy_codec(encoder);
    opj_image_destroy(image);

    if (!success) {
        throw std::runtime_error("Jp2Processor: compression pipeline failed");
    }

    Logger::log(LogLevel::Debug, "Exiting recompress for " + output_path.string(), get_name());
}

std::optional<ExtractedContent> Jp2Processor::prepare_extraction(const std::filesystem::path& /*input_path*/) {
    return std::nullopt;
}

std::filesystem::path Jp2Processor::finalize_extraction(const ExtractedContent& /*content*/, const ProcessingOptions &/*options*/) {
    return {};
}

std::string Jp2Processor::get_raw_checksum(const std::filesystem::path& /*file_path*/) const {
    return "";
}

} // namespace chisel
