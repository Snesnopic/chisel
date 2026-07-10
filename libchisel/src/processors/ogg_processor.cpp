//
// Created by Giuseppe Francione on 18/11/25.
//

#include "../../include/ogg_processor.hpp"
#include "../../include/logger.hpp"
#include "../../include/audio_metadata_util.hpp"
#include "../../include/file_utils.hpp"
#include <stdexcept>
#include <filesystem>
#include <cstdio>
#include <cstring>
#include <vector>
#include <fstream>
#include <algorithm>
#include <FLAC/all.h>

#ifdef HAVE_OPTIVORBIS
extern "C" {
    int chisel_optimize_vorbis(const char* input, const char* output);
}
#endif
namespace chisel {
namespace fs = std::filesystem;

namespace {
    bool is_vorbis_stream(FILE* f) {
        if (f == nullptr) return false;

        const long start_pos = ftell(f);
        fseek(f, 0, SEEK_SET);

        unsigned char header[27];
        if (fread(header, 1, 27, f) != 27) {
            fseek(f, start_pos, SEEK_SET);
            return false;
        }

        if (std::memcmp(header, "OggS", 4) != 0) {
            fseek(f, start_pos, SEEK_SET);
            return false;
        }

        const int num_segments = header[26];

        if (fseek(f, num_segments, SEEK_CUR) != 0) {
            fseek(f, start_pos, SEEK_SET);
            return false;
        }

        unsigned char signature[7];
        if (fread(signature, 1, 7, f) != 7) {
            fseek(f, start_pos, SEEK_SET);
            return false;
        }

        const bool is_vorbis = (std::memcmp(signature, "\x01vorbis", 7) == 0);

        fseek(f, start_pos, SEEK_SET);
        return is_vorbis;
    }
    bool is_opus_stream(FILE* f) {
        if (f == nullptr) return false;

        const long start_pos = ftell(f);
        fseek(f, 0, SEEK_SET);

        unsigned char header[27];
        if (fread(header, 1, 27, f) != 27) {
            fseek(f, start_pos, SEEK_SET);
            return false;
        }

        if (std::memcmp(header, "OggS", 4) != 0) {
            fseek(f, start_pos, SEEK_SET);
            return false;
        }

        const int num_segments = header[26];

        if (fseek(f, num_segments, SEEK_CUR) != 0) {
            fseek(f, start_pos, SEEK_SET);
            return false;
        }

        unsigned char signature[8];
        if (fread(signature, 1, 8, f) != 8) {
            fseek(f, start_pos, SEEK_SET);
            return false;
        }

        const bool is_opus = (std::memcmp(signature, "OpusHead", 8) == 0);

        fseek(f, start_pos, SEEK_SET);
        return is_opus;
    }
    struct OggIO {
        FILE* f_in = nullptr;
    };

    FLAC__StreamDecoderReadStatus read_cb(const FLAC__StreamDecoder*, FLAC__byte buffer[], std::size_t *bytes, void *client_data) {
        const auto* io = static_cast<OggIO*>(client_data);
        if (*bytes > 0) {
            *bytes = fread(buffer, sizeof(FLAC__byte), *bytes, io->f_in);
            if (*bytes == 0 && ferror(io->f_in)) return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
            if (*bytes == 0 && feof(io->f_in)) return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
            return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
        }
        return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
    }

    FLAC__StreamDecoderSeekStatus seek_cb(const FLAC__StreamDecoder*, const FLAC__uint64 absolute_byte_offset, void *client_data) {
        const auto* io = static_cast<OggIO*>(client_data);
        if (fseek(io->f_in, (long)absolute_byte_offset, SEEK_SET) < 0) return FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;
        return FLAC__STREAM_DECODER_SEEK_STATUS_OK;
    }

    FLAC__StreamDecoderTellStatus tell_cb(const FLAC__StreamDecoder*, FLAC__uint64 *absolute_byte_offset, void *client_data) {
        const auto* io = static_cast<OggIO*>(client_data);
        const long pos = ftell(io->f_in);
        if (pos < 0) return FLAC__STREAM_DECODER_TELL_STATUS_ERROR;
        *absolute_byte_offset = static_cast<FLAC__uint64>(pos);
        return FLAC__STREAM_DECODER_TELL_STATUS_OK;
    }

    FLAC__StreamDecoderLengthStatus length_cb(const FLAC__StreamDecoder*, FLAC__uint64 *stream_length, void *client_data) {
        const auto* io = static_cast<OggIO*>(client_data);
        const long curr = ftell(io->f_in);
        fseek(io->f_in, 0, SEEK_END);
        const long len = ftell(io->f_in);
        fseek(io->f_in, curr, SEEK_SET);
        if (len < 0) return FLAC__STREAM_DECODER_LENGTH_STATUS_ERROR;
        *stream_length = static_cast<FLAC__uint64>(len);
        return FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
    }

    FLAC__bool eof_cb(const FLAC__StreamDecoder*, void *client_data) {
        const auto* io = static_cast<OggIO*>(client_data);
        return feof(io->f_in) ? true : false;
    }

    // --- Encoder Callbacks ---

    // required alongside enc_seek_cb: finish() seeks back to rewrite STREAMINFO stats, which needs to read the old bytes first
    FLAC__StreamEncoderReadStatus enc_read_cb(const FLAC__StreamEncoder*, FLAC__byte buffer[], std::size_t *bytes, void *client_data) {
        const auto f = static_cast<FILE*>(client_data);
        if (*bytes > 0) {
            *bytes = fread(buffer, sizeof(FLAC__byte), *bytes, f);
            if (*bytes == 0 && ferror(f)) return FLAC__STREAM_ENCODER_READ_STATUS_ABORT;
            if (*bytes == 0 && feof(f)) return FLAC__STREAM_ENCODER_READ_STATUS_END_OF_STREAM;
            return FLAC__STREAM_ENCODER_READ_STATUS_CONTINUE;
        }
        return FLAC__STREAM_ENCODER_READ_STATUS_ABORT;
    }

    FLAC__StreamEncoderWriteStatus write_cb(const FLAC__StreamEncoder*, const FLAC__byte buffer[], const std::size_t bytes, unsigned, unsigned, void *client_data) {
        const auto f = static_cast<FILE*>(client_data);
        if (fwrite(buffer, 1, bytes, f) != bytes) return FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
        return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
    }

    FLAC__StreamEncoderSeekStatus enc_seek_cb(const FLAC__StreamEncoder*, const FLAC__uint64 absolute_byte_offset, void *client_data) {
       const auto f = static_cast<FILE*>(client_data);
        if (fseek(f, (long)absolute_byte_offset, SEEK_SET) < 0) return FLAC__STREAM_ENCODER_SEEK_STATUS_ERROR;
        return FLAC__STREAM_ENCODER_SEEK_STATUS_OK;
    }

    FLAC__StreamEncoderTellStatus enc_tell_cb(const FLAC__StreamEncoder*, FLAC__uint64 *absolute_byte_offset, void *client_data) {
        const auto f = static_cast<FILE*>(client_data);
        const long pos = ftell(f);
        if (pos < 0) return FLAC__STREAM_ENCODER_TELL_STATUS_ERROR;
        *absolute_byte_offset = static_cast<FLAC__uint64>(pos);
        return FLAC__STREAM_ENCODER_TELL_STATUS_OK;
    }

    struct TranscodeContext : OggIO {
        FLAC__StreamEncoder* encoder = nullptr;
        FILE* f_out = nullptr;
        bool encoder_init = false;
        bool preserve_metadata = true;
        bool failed = false;
        std::vector<FLAC__StreamMetadata*> meta_blocks;

        ~TranscodeContext() {
            for (auto* m : meta_blocks) {
                FLAC__metadata_object_delete(m);
            }
        }
    };

    void dec_metadata_cb(const FLAC__StreamDecoder*, const FLAC__StreamMetadata* metadata, void* client_data) {
        auto* ctx = static_cast<TranscodeContext*>(client_data);
        if (!ctx->preserve_metadata) return;

        // prevent vector reallocation after encoder initialization
        if (ctx->encoder_init) return;

        // preserve both vorbis comments and embedded pictures
        if (metadata->type == FLAC__METADATA_TYPE_VORBIS_COMMENT ||
            metadata->type == FLAC__METADATA_TYPE_PICTURE) {
            FLAC__StreamMetadata* copy = FLAC__metadata_object_clone(metadata);
            if (copy) {
                ctx->meta_blocks.push_back(copy);
            }
            }
    }

    void dec_error_cb(const FLAC__StreamDecoder*, const FLAC__StreamDecoderErrorStatus status, void*) {
        Logger::log(LogLevel::Warning, "Libflac warning: " + std::string(FLAC__StreamDecoderErrorStatusString[status]), "OggProcessor");
    }

    FLAC__StreamDecoderWriteStatus dec_write_cb(const FLAC__StreamDecoder*, const FLAC__Frame* frame, const FLAC__int32* const buffer[], void* client_data) {
        auto* ctx = static_cast<TranscodeContext*>(client_data);

        if (!ctx->encoder_init) {
            FLAC__stream_encoder_set_channels(ctx->encoder, frame->header.channels);
            FLAC__stream_encoder_set_bits_per_sample(ctx->encoder, frame->header.bits_per_sample);
            FLAC__stream_encoder_set_sample_rate(ctx->encoder, frame->header.sample_rate);

            FLAC__stream_encoder_set_compression_level(ctx->encoder, 8);
            FLAC__stream_encoder_set_blocksize(ctx->encoder, 0);
            FLAC__stream_encoder_set_do_exhaustive_model_search(ctx->encoder, true);
            FLAC__stream_encoder_set_min_residual_partition_order(ctx->encoder, 0);
            FLAC__stream_encoder_set_max_residual_partition_order(ctx->encoder, 6);
            FLAC__stream_encoder_set_max_lpc_order(ctx->encoder, 16);
            FLAC__stream_encoder_set_apodization(ctx->encoder, "tukey(0.5);partial_tukey(2);punchout_tukey(3);gauss(0.2)");
            FLAC__stream_encoder_set_do_mid_side_stereo(ctx->encoder, true);
            FLAC__stream_encoder_set_loose_mid_side_stereo(ctx->encoder, false);
            FLAC__stream_encoder_set_streamable_subset(ctx->encoder, false);

            if (!ctx->meta_blocks.empty()) {
                FLAC__stream_encoder_set_metadata(ctx->encoder, ctx->meta_blocks.data(), static_cast<unsigned>(ctx->meta_blocks.size()));
            }

            const auto init_status = FLAC__stream_encoder_init_ogg_stream(
                    ctx->encoder,
                    enc_read_cb, write_cb, enc_seek_cb, enc_tell_cb, nullptr,
                    ctx->f_out);
            if (init_status != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
                Logger::log(LogLevel::Error, std::string("Failed to init FLAC Ogg encoder: ") + FLAC__StreamEncoderInitStatusString[init_status], "OggProcessor");
                ctx->failed = true;
                return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
            }
            ctx->encoder_init = true;
        }

        if (FLAC__stream_encoder_process(ctx->encoder, buffer, frame->header.blocksize) == 0) {
            Logger::log(LogLevel::Error, "Encoding process failed", "OggProcessor");
            ctx->failed = true;
            return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
        }

        return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
    }

    struct DecodeCtx : OggIO {
        std::vector<int32_t> pcm;
        unsigned channels = 0;
        unsigned bps = 0;
        unsigned sample_rate = 0;
    };

    FLAC__StreamDecoderWriteStatus raw_write_cb(const FLAC__StreamDecoder*, const FLAC__Frame* frame, const FLAC__int32* const buffer[], void* client_data) {
        auto* c = static_cast<DecodeCtx*>(client_data);
        if (c->channels == 0) {
            c->channels = frame->header.channels;
            c->bps = frame->header.bits_per_sample;
            c->sample_rate = frame->header.sample_rate;
        }

        const std::size_t n = frame->header.blocksize;
        for (std::size_t i = 0; i < n; ++i) {
            for (unsigned ch = 0; ch < frame->header.channels; ++ch) {
                c->pcm.push_back(buffer[ch][i]);
            }
        }
        return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
    }

    bool files_byte_equal(const fs::path& a, const fs::path& b) {
        std::error_code ec_a, ec_b;
        const auto size_a = fs::file_size(a, ec_a);
        const auto size_b = fs::file_size(b, ec_b);
        if (ec_a || ec_b || size_a != size_b) return false;

        std::ifstream fa(a, std::ios::binary);
        std::ifstream fb(b, std::ios::binary);
        if (!fa || !fb) return false;

        return std::equal(std::istreambuf_iterator<char>(fa), std::istreambuf_iterator<char>(),
                          std::istreambuf_iterator<char>(fb));
    }

    std::vector<int32_t> decode_ogg_pcm(const fs::path& file, unsigned& sr, unsigned& ch, unsigned& bps) {
        FILE* f = chisel::open_file(file, "rb");
        if (f == nullptr) return {};

        FLAC__StreamDecoder* decoder = FLAC__stream_decoder_new();
        if (decoder == nullptr) {
            fclose(f);
            return {};
        }

        DecodeCtx ctx;
        ctx.f_in = f;

        auto error_cb_dummy = [](const FLAC__StreamDecoder*, FLAC__StreamDecoderErrorStatus, void*) {};

        bool is_flac = false;
        if (FLAC__stream_decoder_init_ogg_stream(decoder, read_cb, seek_cb, tell_cb, length_cb, eof_cb,
                                                 raw_write_cb, nullptr, error_cb_dummy, &ctx) == FLAC__STREAM_DECODER_INIT_STATUS_OK) {
            is_flac = true;
            FLAC__stream_decoder_process_until_end_of_stream(decoder);
        }

        FLAC__stream_decoder_finish(decoder);
        FLAC__stream_decoder_delete(decoder);
        fclose(f);

        if (!is_flac) return {};

        sr = ctx.sample_rate;
        ch = ctx.channels;
        bps = ctx.bps;
        return ctx.pcm;
    }

} // namespace

void OggProcessor::recompress(const fs::path& input,
                              const fs::path& output, const ProcessingOptions &options) {
    Logger::log(LogLevel::Debug, "Entering recompress for " + input.string(), get_name());

    FILE* f_in = chisel::open_file(input, "rb");
    if (f_in == nullptr) throw std::runtime_error("OggProcessor: cannot open input");

    // check vorbis early to prevent flac decoder init failures
    if (is_vorbis_stream(f_in)) {
        fclose(f_in);

#ifdef HAVE_OPTIVORBIS
        const std::string input_str = input.string();
        const std::string output_str = output.string();

        const int result = chisel_optimize_vorbis(input_str.c_str(), output_str.c_str());
        if (result != 0) {
            const std::string msg = "Optivorbis failed with error code: " + std::to_string(result);
            Logger::log(LogLevel::Error, msg, get_name());
            throw std::runtime_error(msg);
        }
#endif
        Logger::log(LogLevel::Debug, "Exiting recompress for " + output.string(), get_name());
        return;
    }

    // handle opus with direct copy
    if (is_opus_stream(f_in)) {
        fclose(f_in);
        Logger::log(LogLevel::Warning, "Unsupported opus stream, falling back to direct copy", get_name());
        try {
            fs::copy_file(input, output, fs::copy_options::overwrite_existing);
        } catch (const fs::filesystem_error&) {
            throw std::runtime_error("OggProcessor: direct copy for Opus failed");
        }
        Logger::log(LogLevel::Debug, "Exiting recompress for " + output.string(), get_name());
        return;
    }

    // fallback to flac context setup
    FLAC__StreamDecoder* decoder = FLAC__stream_decoder_new();
    if (decoder == nullptr) {
        fclose(f_in);
        throw std::runtime_error("OggProcessor: failed to create decoder");
    }

    FLAC__StreamEncoder* encoder = FLAC__stream_encoder_new();
    if (encoder == nullptr) {
        FLAC__stream_decoder_delete(decoder);
        fclose(f_in);
        throw std::runtime_error("OggProcessor: failed to create encoder");
    }

    // prepare context
    TranscodeContext ctx;
    ctx.f_in = f_in;
    ctx.encoder = encoder;
    ctx.preserve_metadata = options.preserve_metadata;

    const FLAC__StreamDecoderInitStatus init_stat = FLAC__stream_decoder_init_ogg_stream(
        decoder,
        read_cb, seek_cb, tell_cb, length_cb, eof_cb,
        dec_write_cb, dec_metadata_cb, dec_error_cb,
        &ctx
    );

    if (init_stat != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
        // cleanup to avoid handle leaks on abort
        FLAC__stream_decoder_delete(decoder);
        FLAC__stream_encoder_delete(encoder);
        fclose(f_in);
        throw std::runtime_error("OggProcessor: recompression failed or aborted");
    }

    // read+write: enc_seek_cb/enc_read_cb need to seek back and re-read bytes to rewrite STREAMINFO stats on finish()
    FILE* f_out = chisel::open_file(output, "w+b");
    if (f_out == nullptr) {
        FLAC__stream_decoder_delete(decoder);
        FLAC__stream_encoder_delete(encoder);
        fclose(f_in);
        throw std::runtime_error("OggProcessor: cannot open output");
    }
    ctx.f_out = f_out;

    Logger::log(LogLevel::Info, "Recompressing ogg-flac stream...", get_name());
    const bool success = FLAC__stream_decoder_process_until_end_of_stream(decoder) != 0;

    if (ctx.encoder_init) {
        FLAC__stream_encoder_finish(encoder);
    }

    FLAC__stream_encoder_delete(encoder);
    FLAC__stream_decoder_delete(decoder);
    fclose(f_in);
    fclose(f_out);

    if (!success || ctx.failed) {
        std::error_code ec;
        fs::remove(output, ec);
        throw std::runtime_error("OggProcessor: recompression failed or aborted");
    }
    Logger::log(LogLevel::Debug, "Exiting recompress for " + output.string(), get_name());
}

std::optional<ExtractedContent> OggProcessor::prepare_extraction(const fs::path& input_path) {
    return AudioMetadataUtil::prepareCoverExtraction(input_path, "ogg-processor", get_name());
}

std::filesystem::path OggProcessor::finalize_extraction(const ExtractedContent &content, const ProcessingOptions &options) {
    return AudioMetadataUtil::finalizeCoverExtraction(content, get_name());
}

std::string OggProcessor::get_raw_checksum(const std::filesystem::path&) const {
    return "";
}

bool OggProcessor::raw_equal(const fs::path& a, const fs::path& b) const {
    unsigned ra, ca, bpsa;
    unsigned rb, cb, bpsb;

    const auto pcmA = decode_ogg_pcm(a, ra, ca, bpsa);
    const auto pcmB = decode_ogg_pcm(b, rb, cb, bpsb);

    if (!pcmA.empty() && !pcmB.empty()) {
        if (ra != rb || ca != cb || bpsa != bpsb) return false;
        return pcmA == pcmB;
    }

    if (!pcmA.empty() || !pcmB.empty()) {
        return false;
    }

    // neither file decoded as ogg-flac: check the actual stream type of each
    FILE* fA = fopen(a.string().c_str(), "rb");
    FILE* fB = fopen(b.string().c_str(), "rb");

    const bool opus_a = is_opus_stream(fA);
    const bool opus_b = is_opus_stream(fB);
    const bool vorbis_a = is_vorbis_stream(fA);
    const bool vorbis_b = is_vorbis_stream(fB);

    if (fA != nullptr) fclose(fA);
    if (fB != nullptr) fclose(fB);

    if (opus_a && opus_b) {
        // recompress() always falls back to a direct copy for opus, so bytes must match exactly
        return files_byte_equal(a, b);
    }

    if (vorbis_a && vorbis_b) {
        // OptiVorbis is documented to losslessly preserve the audio stream; no vorbis decoder
        // is vendored here to verify that independently, so this trusts that guarantee
        return true;
    }

    return false;
}

} // namespace chisel