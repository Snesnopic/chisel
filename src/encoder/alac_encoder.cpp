//
// Created by Giuseppe Francione on 25/09/25.
//

#include "alac_encoder.hpp"
#include "../utils/logger.hpp"
#include <stdexcept>
#include <vector>
#include <fstream>
#include <algorithm>
#include "jpeg_encoder.hpp"
#include "png_encoder.hpp"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
}

namespace {
#pragma pack(push, 1)
    struct RiffHeader {
        char riff[4]; // "RIFF"
        uint32_t size;
        char wave[4]; // "WAVE"
    };

    struct ChunkHeader {
        char id[4];
        uint32_t size;
    };

    struct FmtPcm {
        uint16_t audio_format; // 1 = PCM
        uint16_t num_channels;
        uint32_t sample_rate;
        uint32_t byte_rate;
        uint16_t block_align;
        uint16_t bits_per_sample;
    };
#pragma pack(pop)

    struct WavInfo {
        uint16_t channels = 0;
        uint32_t sample_rate = 0;
        uint16_t bits_per_sample = 0;
        std::vector<uint8_t> pcm; // interleaved samples
        std::string title;
        std::string artist;
        std::string album;
        std::string year;
        std::string comment;
    };

    // read a little-endian 32-bit safely
    static uint32_t le32(const uint32_t v) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        return v;
#else
        return ((v & 0x000000FFu) << 24) |
               ((v & 0x0000FF00u) << 8) |
               ((v & 0x00FF0000u) >> 8) |
               ((v & 0xFF000000u) >> 24);
#endif
    }


    // minimal wav loader (pcm 16/24-bit, mono/stereo, riff info tags)
    WavInfo load_wav_pcm(const std::filesystem::path &input) {
        WavInfo info{};

        std::ifstream f(input, std::ios::binary);
        if (!f) throw std::runtime_error("cannot open wav input");

        RiffHeader rh{};
        f.read(reinterpret_cast<char *>(&rh), sizeof(rh));
        if (!f || std::memcmp(rh.riff, "RIFF", 4) != 0 || std::memcmp(rh.wave, "WAVE", 4) != 0) {
            throw std::runtime_error("not a riff/wave file");
        }

        bool have_fmt = false;
        bool have_data = false;

        while (f) {
            ChunkHeader ch{};
            f.read(reinterpret_cast<char *>(&ch), sizeof(ch));
            if (!f) break;

            const uint32_t size = le32(ch.size);

            if (std::memcmp(ch.id, "fmt ", 4) == 0) {
                if (size < sizeof(FmtPcm)) throw std::runtime_error("invalid fmt chunk");
                FmtPcm fmt{};
                f.read(reinterpret_cast<char *>(&fmt), sizeof(fmt));
                if (!f) throw std::runtime_error("failed reading fmt chunk");
                if (fmt.audio_format != 1) throw std::runtime_error("only pcm wav supported");
                info.channels = fmt.num_channels;
                info.sample_rate = fmt.sample_rate;
                info.bits_per_sample = fmt.bits_per_sample;
                if (size > sizeof(FmtPcm)) f.seekg(static_cast<std::streamoff>(size - sizeof(FmtPcm)), std::ios::cur);
                have_fmt = true;
            } else if (std::memcmp(ch.id, "data", 4) == 0) {
                if (!have_fmt) throw std::runtime_error("data chunk before fmt");
                info.pcm.resize(size);
                f.read(reinterpret_cast<char *>(info.pcm.data()), size);
                if (!f) throw std::runtime_error("failed reading pcm data");
                have_data = true;
            } else if (std::memcmp(ch.id, "LIST", 4) == 0) {
                std::streampos list_end = f.tellg();
                list_end += static_cast<std::streamoff>(size);
                char list_type[4]{};
                f.read(list_type, 4);
                if (std::memcmp(list_type, "INFO", 4) == 0) {
                    while (f && f.tellg() < list_end) {
                        ChunkHeader info_ch{};
                        f.read(reinterpret_cast<char *>(&info_ch), sizeof(info_ch));
                        if (!f) break;
                        uint32_t isz = le32(info_ch.size);
                        std::string val;
                        val.resize(isz);
                        f.read(val.data(), isz);
                        if (!val.empty() && val.back() == '\0') val.pop_back();

                        if (std::memcmp(info_ch.id, "INAM", 4) == 0) info.title = val;
                        else if (std::memcmp(info_ch.id, "IART", 4) == 0) info.artist = val;
                        else if (std::memcmp(info_ch.id, "IPRD", 4) == 0) info.album = val;
                        else if (std::memcmp(info_ch.id, "ICRD", 4) == 0) info.year = val;
                        else if (std::memcmp(info_ch.id, "ICMT", 4) == 0) info.comment = val;

                        if (isz & 1u) f.seekg(1, std::ios::cur);
                    }
                } else {
                    f.seekg(list_end, std::ios::beg);
                }
            } else {
                f.seekg(static_cast<std::streamoff>(size), std::ios::cur);
            }

            if (size & 1u) f.seekg(1, std::ios::cur);
        }

        if (!have_fmt || !have_data) throw std::runtime_error("missing fmt or data");

        if (!(info.channels == 1 || info.channels == 2)) {
            throw std::runtime_error("only mono/stereo supported");
        }
        if (!(info.bits_per_sample == 16 || info.bits_per_sample == 24)) {
            throw std::runtime_error("only 16/24-bit pcm supported");
        }

        return info;
    }

    // choose best sample format supported by ffmpeg alac encoder
    AVSampleFormat choose_alac_sample_fmt(const AVCodec *codec, int bps) {
        constexpr AVSampleFormat preferred[4] = {
            AV_SAMPLE_FMT_S32P, AV_SAMPLE_FMT_S32, AV_SAMPLE_FMT_S16P, AV_SAMPLE_FMT_S16
        };
        if (!codec->sample_fmts) {
            // fallback based on bits per sample
            return bps == 24 ? AV_SAMPLE_FMT_S32P : AV_SAMPLE_FMT_S16P;
        }
        // try to pick the best matching for 24-bit (→ s32) or 16-bit (→ s16)
        for (int i = 0; codec->sample_fmts[i] != AV_SAMPLE_FMT_NONE; ++i) {
            AVSampleFormat fmt = codec->sample_fmts[i];
            for (AVSampleFormat want: preferred) {
                if (bps == 24 && (want == AV_SAMPLE_FMT_S32P || want == AV_SAMPLE_FMT_S32) && fmt == want) return want;
                if (bps == 16 && (want == AV_SAMPLE_FMT_S16P || want == AV_SAMPLE_FMT_S16) && fmt == want) return want;
            }
        }
        // otherwise pick the first advertised
        return codec->sample_fmts[0];
    }

    // fill an AVFrame with planar samples converted from interleaved wav
    void fill_frame_planar(const WavInfo &wav, const AVFrame *frame, size_t start_sample, int nb_samples) {
        const int chs = wav.channels;
        const int sample_bytes = wav.bits_per_sample / 8;

        for (int c = 0; c < chs; ++c) {
            uint8_t *dst = frame->data[c];
            for (int i = 0; i < nb_samples; ++i) {
                const size_t off = (start_sample + i) * chs + c;
                const uint8_t *p = wav.pcm.data() + off * sample_bytes;
                if (frame->format == AV_SAMPLE_FMT_S16P) {
                    int16_t s = static_cast<int16_t>(p[0] | (p[1] << 8));
                    std::memcpy(dst + i * 2, &s, 2);
                } else if (frame->format == AV_SAMPLE_FMT_S32P) {
                    int32_t v = (static_cast<int32_t>(p[0])) |
                                (static_cast<int32_t>(p[1]) << 8) |
                                (static_cast<int32_t>(p[2]) << 16);
                    if (v & 0x00800000) v |= 0xFF000000; // sign extend 24-bit
                    std::memcpy(dst + i * 4, &v, 4);
                } else {
                    // other formats not expected here
                }
            }
        }
    }
} // namespace
void insert_cover_art(AVFormatContext *fmt,
                      const std::vector<uint8_t> &img,
                      bool is_png) {
    if (img.empty()) return;

    AVStream *cover_st = avformat_new_stream(fmt, nullptr);
    if (!cover_st) throw std::runtime_error("FAILED TO CREATE COVER STREAM");

    cover_st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    cover_st->codecpar->codec_id = is_png ? AV_CODEC_ID_PNG : AV_CODEC_ID_MJPEG;
    cover_st->disposition |= AV_DISPOSITION_ATTACHED_PIC;

    cover_st->attached_pic.data = (uint8_t *) av_memdup(img.data(), img.size());
    cover_st->attached_pic.size = static_cast<int>(img.size());
    cover_st->attached_pic.stream_index = cover_st->index;
    cover_st->attached_pic.flags |= AV_PKT_FLAG_KEY;
}

AlacEncoder::AlacEncoder(bool preserve_metadata) {
    preserve_metadata_ = preserve_metadata;
}

bool AlacEncoder::recompress(const std::filesystem::path &input,
                             const std::filesystem::path &output) {
    Logger::log(LogLevel::INFO, "starting alac recompression (m4a): " + input.string(), "alac_encoder");

    // parse wav
    WavInfo wav;
    try {
        wav = load_wav_pcm(input);
    } catch (const std::exception &e) {
        Logger::log(LogLevel::ERROR, std::string("wav parse failed: ") + e.what(), "alac_encoder");
        throw;
    }

    // create mp4/m4a container
    AVFormatContext *fmt = nullptr;
    if (avformat_alloc_output_context2(&fmt, nullptr, "mp4", output.string().c_str()) < 0 || !fmt) {
        Logger::log(LogLevel::ERROR, "avformat_alloc_output_context2 failed (mp4)", "alac_encoder");
        throw std::runtime_error("failed to allocate output context");
    }

    // find alac encoder
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_ALAC);
    if (!codec) {
        Logger::log(LogLevel::ERROR, "alac encoder not found in ffmpeg", "alac_encoder");
        avformat_free_context(fmt);
        throw std::runtime_error("alac encoder not available");
    }

    // add stream
    AVStream *st = avformat_new_stream(fmt, codec);
    if (!st) {
        Logger::log(LogLevel::ERROR, "avformat_new_stream failed", "alac_encoder");
        avformat_free_context(fmt);
        throw std::runtime_error("failed to create stream");
    }
    st->time_base = AVRational{1, static_cast<int>(wav.sample_rate)};

    // allocate codec context
    AVCodecContext *c = avcodec_alloc_context3(codec);
    if (!c) {
        Logger::log(LogLevel::ERROR, "avcodec_alloc_context3 failed", "alac_encoder");
        avformat_free_context(fmt);
        throw std::runtime_error("failed to allocate codec context");
    }

    // channel layout
#if LIBAVUTIL_VERSION_MAJOR >= 57
    av_channel_layout_default(&c->ch_layout, wav.channels);
#else
    c->channels = wav.channels;
    c->channel_layout = (wav.channels == 1) ? AV_CH_LAYOUT_MONO : AV_CH_LAYOUT_STEREO;
#endif
    c->sample_rate = static_cast<int>(wav.sample_rate);
    c->time_base = st->time_base;

    // pick sample format
    const AVSampleFormat want_fmt = choose_alac_sample_fmt(codec, wav.bits_per_sample);
    c->sample_fmt = want_fmt;

    // aim for typical alac frame length (not mandatory; encoder can handle variable)
    if (codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE) {
        c->frame_size = 4096; // typical alac frame
    }

    // open encoder
    if (avcodec_open2(c, codec, nullptr) < 0) {
        Logger::log(LogLevel::ERROR, "avcodec_open2 failed", "alac_encoder");
        avcodec_free_context(&c);
        avformat_free_context(fmt);
        throw std::runtime_error("failed to open alac encoder");
    }

    // export codec params to stream
#if LIBAVUTIL_VERSION_MAJOR >= 57
    if (avcodec_parameters_from_context(st->codecpar, c) < 0) {
        Logger::log(LogLevel::ERROR, "avcodec_parameters_from_context failed", "alac_encoder");
        avcodec_free_context(&c);
        avformat_free_context(fmt);
        throw std::runtime_error("failed to set stream params");
    }
#else
    if (avcodec_parameters_from_context(st->codecpar, c) < 0) {
        Logger::log(LogLevel::ERROR, "avcodec_parameters_from_context failed", "alac_encoder");
        avcodec_free_context(&c);
        avformat_free_context(fmt);
        throw std::runtime_error("failed to set stream params");
    }
#endif

    // open io
    if (!(fmt->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&fmt->pb, output.string().c_str(), AVIO_FLAG_WRITE) < 0) {
            Logger::log(LogLevel::ERROR, "avio_open failed: " + output.string(), "alac_encoder");
            avcodec_free_context(&c);
            avformat_free_context(fmt);
            throw std::runtime_error("failed to open output file");
        }
    }

    // set metadata
    if (preserve_metadata_) {
        if (!wav.title.empty()) av_dict_set(&fmt->metadata, "title", wav.title.c_str(), 0);
        if (!wav.artist.empty()) av_dict_set(&fmt->metadata, "artist", wav.artist.c_str(), 0);
        if (!wav.album.empty()) av_dict_set(&fmt->metadata, "album", wav.album.c_str(), 0);
        if (!wav.year.empty()) av_dict_set(&fmt->metadata, "date", wav.year.c_str(), 0);
        if (!wav.comment.empty()) av_dict_set(&fmt->metadata, "comment", wav.comment.c_str(), 0);
    }

    // write header
    if (avformat_write_header(fmt, nullptr) < 0) {
        Logger::log(LogLevel::ERROR, "avformat_write_header failed", "alac_encoder");
        if (fmt->pb) avio_closep(&fmt->pb);
        avcodec_free_context(&c);
        avformat_free_context(fmt);
        throw std::runtime_error("failed to write header");
    }

    // encoding loop
    const int sample_bytes = wav.bits_per_sample / 8;
    const size_t total_samples = wav.pcm.size() / (sample_bytes * wav.channels);
    const int frame_nsamples = c->frame_size > 0 ? c->frame_size : 4096;

    AVFrame *frame = av_frame_alloc();
    frame->format = c->sample_fmt;
#if LIBAVUTIL_VERSION_MAJOR >= 57
    frame->ch_layout = c->ch_layout;
    const int chs = frame->ch_layout.nb_channels;
#else
    frame->channel_layout = c->channel_layout;
    frame->channels = c->channels;
    const int chs = frame->channels;
#endif
    frame->sample_rate = c->sample_rate;

    size_t processed = 0;
    uint64_t frames_out = 0;

    while (processed < total_samples) {
        const int to_process = static_cast<int>(std::min<size_t>(frame_nsamples, total_samples - processed));
        frame->nb_samples = to_process;

        if (av_frame_get_buffer(frame, 0) < 0) {
            Logger::log(LogLevel::ERROR, "av_frame_get_buffer failed", "alac_encoder");
            if (fmt->pb) avio_closep(&fmt->pb);
            av_frame_free(&frame);
            avcodec_free_context(&c);
            avformat_free_context(fmt);
            throw std::runtime_error("failed to alloc frame buffer");
        }

        // handle planar formats
        if (frame->format == AV_SAMPLE_FMT_S16P || frame->format == AV_SAMPLE_FMT_S32P) {
            fill_frame_planar(wav, frame, processed, to_process);
        } else {
            // packed formats: interleave directly
            for (int i = 0; i < to_process; ++i) {
                for (int cidx = 0; cidx < chs; ++cidx) {
                    const size_t off = (processed + i) * chs + cidx;
                    const uint8_t *p = wav.pcm.data() + off * sample_bytes;
                    if (frame->format == AV_SAMPLE_FMT_S16) {
                        int16_t s = static_cast<int16_t>(p[0] | (p[1] << 8));
                        std::memcpy(frame->data[0] + (i * chs + cidx) * 2, &s, 2);
                    } else if (frame->format == AV_SAMPLE_FMT_S32) {
                        int32_t v = (static_cast<int32_t>(p[0])) |
                                    (static_cast<int32_t>(p[1]) << 8) |
                                    (static_cast<int32_t>(p[2]) << 16);
                        if (v & 0x00800000) v |= 0xFF000000;
                        std::memcpy(frame->data[0] + (i * chs + cidx) * 4, &v, 4);
                    }
                }
            }
        }

        // send to encoder
        if (avcodec_send_frame(c, frame) < 0) {
            Logger::log(LogLevel::ERROR, "avcodec_send_frame failed", "alac_encoder");
            if (fmt->pb) avio_closep(&fmt->pb);
            av_frame_unref(frame);
            av_frame_free(&frame);
            avcodec_free_context(&c);
            avformat_free_context(fmt);
            throw std::runtime_error("failed to send frame");
        }
        av_frame_unref(frame);

        // receive packets
        AVPacket pkt;
        av_init_packet(&pkt);
        pkt.data = nullptr;
        pkt.size = 0;

        while (true) {
            const int ret = avcodec_receive_packet(c, &pkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) {
                Logger::log(LogLevel::ERROR, "avcodec_receive_packet failed", "alac_encoder");
                if (fmt->pb) avio_closep(&fmt->pb);
                av_packet_unref(&pkt);
                av_frame_free(&frame);
                avcodec_free_context(&c);
                avformat_free_context(fmt);
                throw std::runtime_error("failed to receive packet");
            }

            av_packet_rescale_ts(&pkt, c->time_base, st->time_base);
            pkt.stream_index = st->index;

            if (av_interleaved_write_frame(fmt, &pkt) < 0) {
                Logger::log(LogLevel::ERROR, "av_interleaved_write_frame failed", "alac_encoder");
                if (fmt->pb) avio_closep(&fmt->pb);
                av_packet_unref(&pkt);
                av_frame_free(&frame);
                avcodec_free_context(&c);
                avformat_free_context(fmt);
                throw std::runtime_error("failed to write packet");
            }

            frames_out++;
            av_packet_unref(&pkt);
        }

        processed += to_process;
    }

    // flush encoder
    if (avcodec_send_frame(c, nullptr) == 0) {
        AVPacket pkt;
        av_init_packet(&pkt);
        pkt.data = nullptr;
        pkt.size = 0;
        while (true) {
            const int ret = avcodec_receive_packet(c, &pkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) break;
            av_packet_rescale_ts(&pkt, c->time_base, st->time_base);
            pkt.stream_index = st->index;
            av_interleaved_write_frame(fmt, &pkt);
            av_packet_unref(&pkt);
        }
    }

    // cover art extraction, recompression and reinsertion

    AVFormatContext *in_fmt = nullptr;
    if (avformat_open_input(&in_fmt, input.string().c_str(), nullptr, nullptr) >= 0) {
        if (avformat_find_stream_info(in_fmt, nullptr) >= 0) {
            for (unsigned i = 0; i < in_fmt->nb_streams; i++) {
                AVStream *av_stream = in_fmt->streams[i];
                if (av_stream->disposition & AV_DISPOSITION_ATTACHED_PIC) {
                    AVPacket pkt = av_stream->attached_pic;

                    // detect format by magic bytes
                    bool is_png = (pkt.size >= 8 && !memcmp(pkt.data, "\x89PNG", 4));
                    std::filesystem::path tmp_in =
                            std::filesystem::temp_directory_path() / (is_png ? "cover_in.png" : "cover_in.jpg");
                    std::filesystem::path tmp_out =
                            std::filesystem::temp_directory_path() / (is_png ? "cover_out.png" : "cover_out.jpg");

                    // write original cover to temp file
                    {
                        std::ofstream ofs(tmp_in, std::ios::binary);
                        ofs.write(reinterpret_cast<const char *>(pkt.data), pkt.size);
                    }

                    // recompress with your encoders
                    bool ok = false;
                    if (is_png) {
                        PngEncoder a;
                        ok = a.recompress(tmp_in, tmp_out);
                    } else {
                        JpegEncoder a;
                        ok = a.recompress(tmp_in, tmp_out);
                    }

                    if (ok) {
                        // read recompressed file into memory
                        std::ifstream ifs(tmp_out, std::ios::binary);
                        std::vector<uint8_t> cover_data((std::istreambuf_iterator<char>(ifs)),
                                                        std::istreambuf_iterator<char>());

                        if (!cover_data.empty()) {
                            insert_cover_art(fmt, cover_data, is_png);
                            Logger::log(LogLevel::INFO, "INSERTED COVER ART", "alac_encoder");
                        }
                        // cleanup temp files
                        try {
                            std::filesystem::remove(tmp_in);
                            std::filesystem::remove(tmp_out);
                        } catch (const std::exception &e) {
                            Logger::log(LogLevel::WARNING, std::string("FAILED TO REMOVE TEMP FILES: ") + e.what(),
                                        "alac_encoder");
                        }
                    } else {
                        Logger::log(LogLevel::ERROR, "COVER RECOMPRESSION FAILED", "alac_encoder");
                    }
                    break; // only one cover expected
                }
            }
        }
        avformat_close_input(&in_fmt);
    }

    // finalize
    av_write_trailer(fmt);
    if (fmt->pb) avio_closep(&fmt->pb);
    av_frame_free(&frame);
    avcodec_free_context(&c);
    avformat_free_context(fmt);

    Logger::log(LogLevel::INFO, "alac recompression completed (m4a): " + output.string(), "alac_encoder");
    return true;
}
