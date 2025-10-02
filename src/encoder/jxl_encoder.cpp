//
// Created by Giuseppe Francione on 30/09/25.
//

#include "jxl_encoder.hpp"
#include "../utils/logger.hpp"

#include <jxl/encode.h>
#include <jxl/decode.h>
#include <fstream>
#include <vector>
#include <iterator>
#include <string>

// helper to read file into buffer
static bool read_file(const std::filesystem::path &path, std::vector<uint8_t> &buf) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    buf.assign(std::istreambuf_iterator<char>(in), {});
    return true;
}

// helper to write buffer into file
static bool write_file(const std::filesystem::path &path, const std::vector<uint8_t> &buf) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(buf.data()), buf.size());
    return true;
}

// constructor
JXLEncoder::JXLEncoder(const bool preserve_metadata) {
    preserve_metadata_ = preserve_metadata;
}

// recompress implementation
bool JXLEncoder::recompress(const std::filesystem::path &input,
                            const std::filesystem::path &output) {
    Logger::log(LogLevel::INFO, "Re-encoding " + input.string(), "JXL");

    // read input file
    std::vector<uint8_t> input_buf;
    if (!read_file(input, input_buf)) {
        Logger::log(LogLevel::ERROR, "Failed to read input file", "JXL");
        return false;
    }

    // decoder setup
    JxlDecoder* dec = JxlDecoderCreate(nullptr);
    if (!dec) {
        Logger::log(LogLevel::ERROR, "Failed to create decoder", "JXL");
        return false;
    }

    JxlDecoderSubscribeEvents(dec,
        JXL_DEC_BASIC_INFO | JXL_DEC_FRAME | JXL_DEC_FULL_IMAGE | JXL_DEC_BOX);
    JxlDecoderSetInput(dec, input_buf.data(), input_buf.size());
    JxlDecoderCloseInput(dec);

    JxlBasicInfo info{};
    bool ok = true;

    // decoded frames
    struct FrameData {
        std::vector<uint8_t> pixels;
        JxlFrameHeader header{};
    };
    std::vector<FrameData> frames;

    size_t stride = 0;
    constexpr JxlPixelFormat format = {4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};

    // decode loop
    for (;;) {
        const JxlDecoderStatus status = JxlDecoderProcessInput(dec);
        if (status == JXL_DEC_ERROR) {
            Logger::log(LogLevel::ERROR, "Decode error", "JXL");
            ok = false;
            break;
        }
        if (status == JXL_DEC_BASIC_INFO) {
            if (JXL_DEC_SUCCESS != JxlDecoderGetBasicInfo(dec, &info)) {
                Logger::log(LogLevel::ERROR, "Failed to get basic info", "JXL");
                ok = false;
                break;
            }
            stride = info.xsize * 4;
        }
        if (status == JXL_DEC_FRAME) {
            JxlFrameHeader header;
            if (JXL_DEC_SUCCESS != JxlDecoderGetFrameHeader(dec, &header)) {
                Logger::log(LogLevel::ERROR, "Failed to get frame header", "JXL");
                ok = false;
                break;
            }
            FrameData frame;
            frame.header = header;
            frame.pixels.resize(stride * info.ysize);
            if (JXL_DEC_SUCCESS != JxlDecoderSetImageOutBuffer(dec, &format,
                                                              frame.pixels.data(),
                                                              frame.pixels.size())) {
                Logger::log(LogLevel::ERROR, "Failed to set output buffer", "JXL");
                ok = false;
                break;
            }
            frames.push_back(std::move(frame));
        }
        if (status == JXL_DEC_BOX) {
            // TODO:
            // box events not handled in this libjxl version
            // fallback: rely only on JxlEncoderStoreJPEGMetadata
        }
        if (status == JXL_DEC_SUCCESS) {
            break;
        }
    }
    JxlDecoderDestroy(dec);
    if (!ok) return false;

    // encoder setup
    JxlEncoder* enc = JxlEncoderCreate(nullptr);
    if (!enc) {
        Logger::log(LogLevel::ERROR, "Failed to create encoder", "JXL");
        return false;
    }

    // add frames
    for (const auto &frame : frames) {
        JxlEncoderFrameSettings* frame_settings = JxlEncoderFrameSettingsCreate(enc, nullptr);
        JxlEncoderSetFrameLossless(frame_settings, JXL_TRUE);
        JxlEncoderFrameSettingsSetOption(frame_settings,
                                         JXL_ENC_FRAME_SETTING_EFFORT, 9);

        if (JXL_ENC_SUCCESS != JxlEncoderAddImageFrame(frame_settings, &format,
                                                       frame.pixels.data(),
                                                       frame.pixels.size())) {
            Logger::log(LogLevel::ERROR, "Failed to add image frame", "JXL");
            JxlEncoderDestroy(enc);
            return false;
        }
    }

    if (preserve_metadata_) {
        JxlEncoderStoreJPEGMetadata(enc, JXL_TRUE);
    }

    JxlEncoderCloseInput(enc);

    // dynamic buffer loop
    std::vector<uint8_t> out_buf(1 << 20);
    uint8_t* next_out = out_buf.data();
    size_t avail_out = out_buf.size();
    JxlEncoderStatus enc_status;
    while ((enc_status = JxlEncoderProcessOutput(enc, &next_out, &avail_out))
           == JXL_ENC_NEED_MORE_OUTPUT) {
        size_t offset = next_out - out_buf.data();
        out_buf.resize(out_buf.size() * 2);
        next_out = out_buf.data() + offset;
        avail_out = out_buf.size() - offset;
    }
    if (enc_status != JXL_ENC_SUCCESS) {
        Logger::log(LogLevel::ERROR, "Encode error", "JXL");
        JxlEncoderDestroy(enc);
        return false;
    }
    size_t out_size = next_out - out_buf.data();
    out_buf.resize(out_size);
    if (!write_file(output, out_buf)) {
        Logger::log(LogLevel::ERROR, "Failed to write output file", "JXL");
        JxlEncoderDestroy(enc);
        return false;
    }

    JxlEncoderDestroy(enc);
    Logger::log(LogLevel::INFO, "Re-encoding complete", "JXL");
    return true;
}