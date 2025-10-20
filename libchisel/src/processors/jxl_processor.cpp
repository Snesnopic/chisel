//
// Created by Giuseppe Francione on 19/10/25.
//

#include "../../include/jxl_processor.hpp"
#include "../../include/logger.hpp"
#include <jxl/encode.h>
#include <jxl/decode.h>
#include <fstream>
#include <vector>
#include <iterator>
#include <string>

namespace {

// helper to read file into buffer
bool read_file(const std::filesystem::path &path, std::vector<uint8_t> &buf) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    buf.assign(std::istreambuf_iterator<char>(in), {});
    return true;
}

// helper to write buffer into file
bool write_file(const std::filesystem::path &path, const std::vector<uint8_t> &buf) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(buf.data()), buf.size());
    return true;
}

} // namespace

namespace chisel {

void JxlProcessor::recompress(const std::filesystem::path& input,
                              const std::filesystem::path& output,
                              bool preserve_metadata) {
    Logger::log(LogLevel::Info, "Re-encoding " + input.string(), "jxl_processor");

    // read input file
    std::vector<uint8_t> input_buf;
    if (!read_file(input, input_buf)) {
        Logger::log(LogLevel::Error, "Failed to read input file", "jxl_processor");
        throw std::runtime_error("JxlProcessor: cannot read input");
    }

    // decoder setup
    JxlDecoder* dec = JxlDecoderCreate(nullptr);
    if (!dec) throw std::runtime_error("JxlProcessor: cannot create decoder");

    JxlDecoderSubscribeEvents(dec,
        JXL_DEC_BASIC_INFO | JXL_DEC_FRAME | JXL_DEC_FULL_IMAGE | JXL_DEC_BOX);
    JxlDecoderSetInput(dec, input_buf.data(), input_buf.size());
    JxlDecoderCloseInput(dec);

    JxlBasicInfo info{};
    bool ok = true;

    struct FrameData {
        std::vector<uint8_t> pixels;
        JxlFrameHeader header{};
    };
    std::vector<FrameData> frames;

    size_t stride = 0;
    constexpr JxlPixelFormat format = {4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};

    for (;;) {
        const JxlDecoderStatus status = JxlDecoderProcessInput(dec);
        if (status == JXL_DEC_ERROR) { ok = false; break; }
        if (status == JXL_DEC_BASIC_INFO) {
            if (JXL_DEC_SUCCESS != JxlDecoderGetBasicInfo(dec, &info)) { ok = false; break; }
            stride = info.xsize * 4;
        }
        if (status == JXL_DEC_FRAME) {
            JxlFrameHeader header;
            if (JXL_DEC_SUCCESS != JxlDecoderGetFrameHeader(dec, &header)) { ok = false; break; }
            FrameData frame;
            frame.header = header;
            frame.pixels.resize(stride * info.ysize);
            if (JXL_DEC_SUCCESS != JxlDecoderSetImageOutBuffer(dec, &format,
                                                              frame.pixels.data(),
                                                              frame.pixels.size())) { ok = false; break; }
            frames.push_back(std::move(frame));
        }
        if (status == JXL_DEC_SUCCESS) break;
    }
    JxlDecoderDestroy(dec);
    if (!ok) throw std::runtime_error("JxlProcessor: decode failed");

    // encoder setup
    JxlEncoder* enc = JxlEncoderCreate(nullptr);
    if (!enc) throw std::runtime_error("JxlProcessor: cannot create encoder");

    for (const auto &frame : frames) {
        JxlEncoderFrameSettings* frame_settings = JxlEncoderFrameSettingsCreate(enc, nullptr);
        JxlEncoderSetFrameLossless(frame_settings, JXL_TRUE);
        JxlEncoderFrameSettingsSetOption(frame_settings,
                                         JXL_ENC_FRAME_SETTING_EFFORT, 9);

        if (JXL_ENC_SUCCESS != JxlEncoderAddImageFrame(frame_settings, &format,
                                                       frame.pixels.data(),
                                                       frame.pixels.size())) {
            JxlEncoderDestroy(enc);
            throw std::runtime_error("JxlProcessor: failed to add frame");
        }
    }

    if (preserve_metadata) {
        JxlEncoderStoreJPEGMetadata(enc, JXL_TRUE);
    }

    JxlEncoderCloseInput(enc);

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
        JxlEncoderDestroy(enc);
        throw std::runtime_error("JxlProcessor: encode failed");
    }
    size_t out_size = next_out - out_buf.data();
    out_buf.resize(out_size);
    if (!write_file(output, out_buf)) {
        JxlEncoderDestroy(enc);
        throw std::runtime_error("JxlProcessor: cannot write output");
    }

    JxlEncoderDestroy(enc);
    Logger::log(LogLevel::Info, "Re-encoding complete: " + output.string(), "jxl_processor");
}

std::string JxlProcessor::get_raw_checksum(const std::filesystem::path&) const {
    // TODO: implement checksum of raw pixel data
    return "";
}

} // namespace chisel