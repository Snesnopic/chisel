//
// Created by Giuseppe Francione on 19/10/25.
//

#include "jxl_processor.hpp"
#include "logger.hpp"
#include <jxl/encode.h>
#include <jxl/decode.h>
#include "file_utils.hpp"
#include <fstream>
#include <vector>
#include <iterator>
#include <string>


namespace {

size_t get_bytes_per_channel(const JxlDataType data_type) {
    if (data_type == JXL_TYPE_FLOAT) return 4;
    if (data_type == JXL_TYPE_UINT16) return 2;
    return 1; // JXL_TYPE_UINT8
}

} // namespace

namespace chisel {

void JxlProcessor::recompress(const std::filesystem::path& input,
                              const std::filesystem::path& output, const ProcessingOptions &options) {
    Logger::log(LogLevel::Debug, "Entering recompress for " + input.string(), get_name());

    // read input file
    std::vector<uint8_t> input_buf;
    if (!chisel::read_file(input, input_buf)) {
        Logger::log(LogLevel::Error, "Failed to read input file", get_name());
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

    std::size_t stride = 0;

    JxlPixelFormat format = {};
    JxlDataType data_type = {};

    // this will store both the type and the content of each metadata box
    std::vector<std::pair<std::string, std::vector<uint8_t>>> metadata_boxes;
    for (;;) {
        const JxlDecoderStatus status = JxlDecoderProcessInput(dec);
        if (status == JXL_DEC_ERROR) { ok = false; break; }
        if (status == JXL_DEC_BASIC_INFO) {
            if (JXL_DEC_SUCCESS != JxlDecoderGetBasicInfo(dec, &info)) { ok = false; break; }

            uint32_t num_channels = info.num_color_channels;
            if (info.alpha_bits > 0) {
                num_channels++;
            }

            if (info.exponent_bits_per_sample > 0) {
                data_type = JXL_TYPE_FLOAT;
            } else if (info.bits_per_sample > 8) {
                data_type = JXL_TYPE_UINT16;
            } else {
                data_type = JXL_TYPE_UINT8;
            }

            format = {num_channels, data_type, JXL_NATIVE_ENDIAN, 0};

            stride = info.xsize * num_channels * get_bytes_per_channel(data_type);
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
        } else if (status == JXL_DEC_BOX) {
            if (!options.preserve_metadata) {
                // advance the decoder past this box, ignoring its content
                if (JXL_DEC_SUCCESS != JxlDecoderProcessInput(dec)) {
                    ok = false;
                    break;
                }
                continue;
            }

            JxlBoxType type;
            if (JXL_DEC_SUCCESS != JxlDecoderGetBoxType(dec, type, JXL_FALSE /* decompressed */)) {
                ok = false;
                break;
            }

            uint64_t box_size;
            if (JXL_DEC_SUCCESS != JxlDecoderGetBoxSizeContents(dec, &box_size)) {
                ok = false;
                break;
            }

            std::vector<uint8_t> box_data(box_size);
            if (JXL_DEC_SUCCESS != JxlDecoderSetBoxBuffer(dec, box_data.data(), box_data.size())) {
                ok = false;
                break;
            }

            if (JXL_DEC_SUCCESS != JxlDecoderProcessInput(dec)) {
                ok = false;
                break;
            }

            metadata_boxes.emplace_back(std::pair(std::string(type,4), std::move(box_data)));
        }
        if (status == JXL_DEC_SUCCESS) break;
    }
    JxlDecoderDestroy(dec);
    if (!ok) throw std::runtime_error("JxlProcessor: decode failed");

    // encoder setup
    JxlEncoder* enc = JxlEncoderCreate(nullptr);
    if (!enc) throw std::runtime_error("JxlProcessor: cannot create encoder");

    if (JXL_ENC_SUCCESS != JxlEncoderSetBasicInfo(enc, &info)) {
        JxlEncoderDestroy(enc);
        throw std::runtime_error("JxlProcessor: failed to set basic info");
    }

    if (options.preserve_metadata) {
        JxlEncoderStoreJPEGMetadata(enc, JXL_TRUE);
        for (const auto& [type, data] : metadata_boxes) {
            if (JXL_ENC_SUCCESS != JxlEncoderAddBox(enc, type.c_str(), data.data(), data.size(), JXL_FALSE)) {
                Logger::log(LogLevel::Warning, "Failed to add metadata box to jxl encoder", get_name());
            }
        }
    }

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

    JxlEncoderCloseInput(enc);

    std::vector<uint8_t> out_buf(1 << 20);
    uint8_t* next_out = out_buf.data();
    std::size_t avail_out = out_buf.size();
    JxlEncoderStatus enc_status;
    while ((enc_status = JxlEncoderProcessOutput(enc, &next_out, &avail_out))
           == JXL_ENC_NEED_MORE_OUTPUT) {
        std::size_t offset = next_out - out_buf.data();
        out_buf.resize(out_buf.size() * 2);
        next_out = out_buf.data() + offset;
        avail_out = out_buf.size() - offset;
    }
    if (enc_status != JXL_ENC_SUCCESS) {
        JxlEncoderDestroy(enc);
        throw std::runtime_error("JxlProcessor: encode failed");
    }
    std::size_t out_size = next_out - out_buf.data();
    out_buf.resize(out_size);
    if (!chisel::write_file(output, out_buf)) {
        JxlEncoderDestroy(enc);
        throw std::runtime_error("JxlProcessor: cannot write output");
    }

    JxlEncoderDestroy(enc);
    Logger::log(LogLevel::Debug, "Exiting recompress for " + output.string(), get_name());
}
    // helper to decode jxl to raw rgba8 buffer
    static bool decode_jxl_rgba8(const std::filesystem::path& path,
                                 uint32_t& width,
                                 uint32_t& height,
                                 std::vector<uint8_t>& buffer)
{
    std::vector<uint8_t> input_buf;
    if (!chisel::read_file(path, input_buf)) {
        return false;
    }

    JxlDecoder* dec = JxlDecoderCreate(nullptr);
    if (!dec) return false;

    // autodestroy
    std::unique_ptr<JxlDecoder, decltype(&JxlDecoderDestroy)> dec_ptr(dec, &JxlDecoderDestroy);

    JxlDecoderSubscribeEvents(dec, JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE);
    JxlDecoderSetInput(dec, input_buf.data(), input_buf.size());
    JxlDecoderCloseInput(dec);

    JxlBasicInfo info{};
    constexpr JxlPixelFormat format = {4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0}; // force rgba8 output

    for (;;) {
        const JxlDecoderStatus status = JxlDecoderProcessInput(dec);
        if (status == JXL_DEC_ERROR) return false;
        if (status == JXL_DEC_BASIC_INFO) {
            if (JXL_DEC_SUCCESS != JxlDecoderGetBasicInfo(dec, &info)) return false;
            width = info.xsize;
            height = info.ysize;
            buffer.resize(static_cast<std::size_t>(width) * height * 4);
            if (JXL_DEC_SUCCESS != JxlDecoderSetImageOutBuffer(dec, &format,
                                                              buffer.data(),
                                                              buffer.size())) {
                return false;
                                                              }
        }
        if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
            // this should not happen if we set it after basic info
            return false;
        }
        if (status == JXL_DEC_FULL_IMAGE) {
            // frame is decoded
        }
        if (status == JXL_DEC_SUCCESS) {
            // all frames decoded
            return true;
        }
    }
}
    bool JxlProcessor::raw_equal(const std::filesystem::path& a,
                             const std::filesystem::path& b) const {
    uint32_t wa, ha;
    uint32_t wb, hb;
    std::vector<uint8_t> imgA, imgB;

    const bool okA = decode_jxl_rgba8(a, wa, ha, imgA);
    const bool okB = decode_jxl_rgba8(b, wb, hb, imgB);

    if (!okA || !okB) {
        return false;
    }

    if (wa != wb || ha != hb) {
        return false;
    }

    return imgA == imgB;
}
std::string JxlProcessor::get_raw_checksum(const std::filesystem::path&) const {
    // TODO: implement checksum of raw pixel data
    return "";
}

} // namespace chisel