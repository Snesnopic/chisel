//
// Created by Giuseppe Francione on 18/09/25.
//

#include "flac_encoder.hpp"
#include "../utils/logger.hpp"
#include <FLAC/all.h>
#include <vector>
#include <stdexcept>
#include <filesystem>
#include <string>

struct TranscodeContext {
    FLAC__StreamEncoder *encoder = nullptr;
    std::filesystem::path output;
    bool encoder_initialized = false;
    bool preserve_metadata = true;
    FLAC__StreamMetadata **metadata_blocks = nullptr;
    unsigned num_blocks = 0;
    bool failed = false;
};

static FLAC__StreamDecoderWriteStatus write_callback(
    const FLAC__StreamDecoder *,
    const FLAC__Frame *frame,
    const FLAC__int32 *const buffer[],
    void *client_data) {
    auto *ctx = static_cast<TranscodeContext *>(client_data);
    if (!ctx->encoder_initialized) {
        FLAC__stream_encoder_set_channels(ctx->encoder, frame->header.channels);
        FLAC__stream_encoder_set_bits_per_sample(ctx->encoder, frame->header.bits_per_sample);
        FLAC__stream_encoder_set_sample_rate(ctx->encoder, frame->header.sample_rate);
        FLAC__stream_encoder_set_compression_level(ctx->encoder, 8);
        FLAC__stream_encoder_set_blocksize(ctx->encoder, 16384);
        FLAC__stream_encoder_set_do_mid_side_stereo(ctx->encoder, true);
        FLAC__stream_encoder_set_loose_mid_side_stereo(ctx->encoder, false);
        FLAC__stream_encoder_set_apodization(ctx->encoder, "tukey(0.5);partial_tukey(2);punchout_tukey(3);gauss(0.2)");
        FLAC__stream_encoder_set_max_lpc_order(ctx->encoder, 16);
        FLAC__stream_encoder_set_min_residual_partition_order(ctx->encoder, 0);
        FLAC__stream_encoder_set_max_residual_partition_order(ctx->encoder, 6);
        FLAC__stream_encoder_set_streamable_subset(ctx->encoder, false);
        if (ctx->preserve_metadata && ctx->metadata_blocks && ctx->num_blocks > 0) {
            FLAC__stream_encoder_set_metadata(ctx->encoder, ctx->metadata_blocks, ctx->num_blocks);
        }
        const auto st = FLAC__stream_encoder_init_file(ctx->encoder, ctx->output.string().c_str(), nullptr, nullptr);
        if (st != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
            Logger::log(LogLevel::Error, std::string("FLAC init error: ") + FLAC__StreamEncoderInitStatusString[st],
                        "flac_encoder");
            ctx->failed = true;
            return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
        }
        ctx->encoder_initialized = true;
        Logger::log(LogLevel::Debug, "encoder initialized", "flac_encoder");
    }

    if (!FLAC__stream_encoder_process(ctx->encoder, buffer, frame->header.blocksize)) {
        Logger::log(LogLevel::Error, "encoder process failed", "flac_encoder");
        ctx->failed = true;
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void metadata_callback(const FLAC__StreamDecoder *, const FLAC__StreamMetadata *, void *) {
}

static void error_callback(const FLAC__StreamDecoder *, FLAC__StreamDecoderErrorStatus status, void *) {
    Logger::log(LogLevel::Warning, std::string("FLAC decoder: ") + FLAC__StreamDecoderErrorStatusString[status],
                "libFLAC");
}

FlacEncoder::FlacEncoder(bool preserve_metadata) {
    preserve_metadata_ = preserve_metadata;
}

bool FlacEncoder::recompress(const std::filesystem::path &input, const std::filesystem::path &output) {
    Logger::log(LogLevel::Info, "Starting FLAC re-encoding: " + input.string(), "flac_encoder");
    if (std::filesystem::exists(output)) {
        std::filesystem::remove(output);
    }

    TranscodeContext ctx;
    ctx.output = output;
    ctx.preserve_metadata = preserve_metadata_;

    if (preserve_metadata_) {
        FLAC__Metadata_Chain *chain = FLAC__metadata_chain_new();
        if (chain && FLAC__metadata_chain_read(chain, input.string().c_str())) {
            FLAC__Metadata_Iterator *it = FLAC__metadata_iterator_new();
            FLAC__metadata_iterator_init(it, chain);
            unsigned count = 0;
            do {
                const FLAC__StreamMetadata *block = FLAC__metadata_iterator_get_block(it);
                if (block && block->type != FLAC__METADATA_TYPE_STREAMINFO) ++count;
            } while (FLAC__metadata_iterator_next(it));
            if (count > 0) {
                ctx.metadata_blocks = static_cast<FLAC__StreamMetadata **>(malloc(
                    sizeof(FLAC__StreamMetadata *) * count));
                ctx.num_blocks = count;
                FLAC__metadata_iterator_init(it, chain);
                unsigned idx = 0;
                do {
                    const FLAC__StreamMetadata *block = FLAC__metadata_iterator_get_block(it);
                    if (block && block->type != FLAC__METADATA_TYPE_STREAMINFO) {
                        ctx.metadata_blocks[idx++] = FLAC__metadata_object_clone(block);
                    }
                } while (FLAC__metadata_iterator_next(it));
            }
            FLAC__metadata_iterator_delete(it);
        }
        if (chain) FLAC__metadata_chain_delete(chain);
    }

    ctx.encoder = FLAC__stream_encoder_new();
    if (!ctx.encoder) {
        Logger::log(LogLevel::Error, "Can't create encoder", "flac_encoder");
        if (ctx.metadata_blocks) {
            for (unsigned i = 0; i < ctx.num_blocks; ++i) FLAC__metadata_object_delete(ctx.metadata_blocks[i]);
            free(ctx.metadata_blocks);
        }
        throw std::runtime_error("Can't create encoder");
    }

    FLAC__StreamDecoder *decoder = FLAC__stream_decoder_new();
    if (!decoder) {
        Logger::log(LogLevel::Error, "Can't create decoder", "flac_encoder");
        FLAC__stream_encoder_delete(ctx.encoder);
        if (ctx.metadata_blocks) {
            for (unsigned i = 0; i < ctx.num_blocks; ++i) FLAC__metadata_object_delete(ctx.metadata_blocks[i]);
            free(ctx.metadata_blocks);
        }
        throw std::runtime_error("Can't create decoder");
    }

    auto ist = FLAC__stream_decoder_init_file(decoder, input.string().c_str(),
                                              write_callback, metadata_callback, error_callback,
                                              &ctx);
    if (ist != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
        Logger::log(LogLevel::Error, "decoder init failed", "flac_encoder");
        FLAC__stream_decoder_delete(decoder);
        FLAC__stream_encoder_delete(ctx.encoder);
        if (ctx.metadata_blocks) {
            for (unsigned i = 0; i < ctx.num_blocks; ++i) FLAC__metadata_object_delete(ctx.metadata_blocks[i]);
            free(ctx.metadata_blocks);
        }
        throw std::runtime_error("decoder init failed");
    }

    if (!FLAC__stream_decoder_process_until_end_of_stream(decoder) || ctx.failed) {
        Logger::log(LogLevel::Error, "decoding or encoding failed", "flac_encoder");
        FLAC__stream_decoder_finish(decoder);
        FLAC__stream_decoder_delete(decoder);
        if (ctx.encoder_initialized) FLAC__stream_encoder_finish(ctx.encoder);
        FLAC__stream_encoder_delete(ctx.encoder);
        if (ctx.metadata_blocks) {
            for (unsigned i = 0; i < ctx.num_blocks; ++i) FLAC__metadata_object_delete(ctx.metadata_blocks[i]);
            free(ctx.metadata_blocks);
        }
        throw std::runtime_error("FLAC transcoding failed");
    }

    FLAC__stream_decoder_finish(decoder);
    FLAC__stream_decoder_delete(decoder);

    if (ctx.encoder_initialized) {
        FLAC__stream_encoder_finish(ctx.encoder);
    }
    FLAC__stream_encoder_delete(ctx.encoder);

    if (ctx.metadata_blocks) {
        for (unsigned i = 0; i < ctx.num_blocks; ++i) FLAC__metadata_object_delete(ctx.metadata_blocks[i]);
        free(ctx.metadata_blocks);
    }

    Logger::log(LogLevel::Info, "FLAC re-encoding completed: " + output.string(), "flac_encoder");
    return true;
}
