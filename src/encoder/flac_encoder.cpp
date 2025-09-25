//
// Created by Giuseppe Francione on 18/09/25.
//

#include "flac_encoder.hpp"
#include "../utils/logger.hpp" // nuovo include per il logging
#include <FLAC/all.h>
#include <vector>
#include <stdexcept>
#include <filesystem>

struct DecodeContext {
    std::vector<int32_t> pcm_data;
    unsigned sample_rate = 0;
    unsigned channels = 0;
    unsigned bits_per_sample = 0;
};

// decoding callback
static FLAC__StreamDecoderWriteStatus write_callback(
    const FLAC__StreamDecoder *,
    const FLAC__Frame *frame,
    const FLAC__int32 *const buffer[],
    void *client_data) {
    auto *ctx = static_cast<DecodeContext *>(client_data);
    if (ctx->channels == 0) {
        ctx->channels = frame->header.channels;
        ctx->sample_rate = frame->header.sample_rate;
        ctx->bits_per_sample = frame->header.bits_per_sample;
    }

    size_t blocksize = frame->header.blocksize;
    for (size_t i = 0; i < blocksize; ++i) {
        for (unsigned ch = 0; ch < ctx->channels; ++ch) {
            ctx->pcm_data.push_back(buffer[ch][i]);
        }
    }
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void metadata_callback(
    const FLAC__StreamDecoder *,
    const FLAC__StreamMetadata *,
    void *) {
}

static void error_callback(
    const FLAC__StreamDecoder *,
    FLAC__StreamDecoderErrorStatus status,
    void *) {
    Logger::log(LogLevel::WARNING,
                std::string("FLAC decoder: ") + FLAC__StreamDecoderErrorStatusString[status],
                "libFLAC");
}

FlacEncoder::FlacEncoder(bool preserve_metadata) {
    preserve_metadata_ = preserve_metadata;
}

bool FlacEncoder::recompress(const std::filesystem::path &input,
                             const std::filesystem::path &output) {
    Logger::log(LogLevel::INFO, "Starting FLAC reencoding: " + input.string(), "flac_encoder");

    if (std::filesystem::exists(output)) {
        std::filesystem::remove(output);
    }

    // decoding
    DecodeContext ctx;
    FLAC__StreamDecoder *decoder = FLAC__stream_decoder_new();
    if (!decoder) {
        Logger::log(LogLevel::ERROR, "Can't create FLAC decoder", "flac_encoder");
        throw std::runtime_error("Can't create FLAC decoder");
    }

    if (FLAC__stream_decoder_init_file(decoder, input.string().c_str(),
                                       write_callback, metadata_callback, error_callback,
                                       &ctx) != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
        Logger::log(LogLevel::ERROR, "FLAC decoder init failed", "flac_encoder");
        FLAC__stream_decoder_delete(decoder);
        throw std::runtime_error("FLAC decoder init failed");
    }

    if (!FLAC__stream_decoder_process_until_end_of_stream(decoder)) {
        Logger::log(LogLevel::ERROR, "FLAC decoding failed", "flac_encoder");
        FLAC__stream_decoder_finish(decoder);
        FLAC__stream_decoder_delete(decoder);
        throw std::runtime_error("FLAC decoding failed");
    }

    FLAC__stream_decoder_finish(decoder);
    FLAC__stream_decoder_delete(decoder);

    if (ctx.channels == 0 || ctx.sample_rate == 0) {
        Logger::log(LogLevel::ERROR, "Invalid FLAC file", "flac_encoder");
        throw std::runtime_error("Invalid FLAC file");
    }

    Logger::log(LogLevel::DEBUG,
                "Params: " + std::to_string(ctx.channels) + "ch, " +
                std::to_string(ctx.sample_rate) + "Hz, " +
                std::to_string(ctx.bits_per_sample) + "bit",
                "flac_encoder");

    // metadata (if requested)
    FLAC__StreamMetadata **metadata_blocks = nullptr;
    unsigned num_blocks = 0;
    if (preserve_metadata_) {
        FLAC__Metadata_Chain *chain = FLAC__metadata_chain_new();
        if (chain && FLAC__metadata_chain_read(chain, input.string().c_str())) {
            FLAC__Metadata_Iterator *it = FLAC__metadata_iterator_new();
            FLAC__metadata_iterator_init(it, chain);

            do {
                FLAC__StreamMetadata const *block = FLAC__metadata_iterator_get_block(it);
                if (block && block->type != FLAC__METADATA_TYPE_STREAMINFO) {
                    ++num_blocks;
                }
            } while (FLAC__metadata_iterator_next(it));

            if (num_blocks > 0) {
                metadata_blocks = static_cast<FLAC__StreamMetadata **>(
                    malloc(sizeof(FLAC__StreamMetadata *) * num_blocks)
                );
                FLAC__metadata_iterator_init(it, chain);
                unsigned idx = 0;
                do {
                    FLAC__StreamMetadata *block = FLAC__metadata_iterator_get_block(it);
                    if (block && block->type != FLAC__METADATA_TYPE_STREAMINFO) {
                        metadata_blocks[idx++] = FLAC__metadata_object_clone(block);
                    }
                } while (FLAC__metadata_iterator_next(it));
            }
            FLAC__metadata_iterator_delete(it);
        }
        if (chain) FLAC__metadata_chain_delete(chain);
    }

    // re-encoding
    FLAC__StreamEncoder *encoder = FLAC__stream_encoder_new();
    if (!encoder) {
        Logger::log(LogLevel::ERROR, "Can't create FLAC encoder", "flac_encoder");
        throw std::runtime_error("Can't create FLAC encoder");
    }

    FLAC__stream_encoder_set_compression_level(encoder, 8);
    FLAC__stream_encoder_set_blocksize(encoder, 16384);
    FLAC__stream_encoder_set_do_mid_side_stereo(encoder, true);
    FLAC__stream_encoder_set_loose_mid_side_stereo(encoder, false);
    FLAC__stream_encoder_set_apodization(encoder, "tukey(0.5);partial_tukey(2);punchout_tukey(3);gauss(0.2)");
    FLAC__stream_encoder_set_max_lpc_order(encoder, 16);
    FLAC__stream_encoder_set_min_residual_partition_order(encoder, 0);
    FLAC__stream_encoder_set_max_residual_partition_order(encoder, 6);

    FLAC__stream_encoder_set_channels(encoder, ctx.channels);
    FLAC__stream_encoder_set_bits_per_sample(encoder, ctx.bits_per_sample);
    FLAC__stream_encoder_set_sample_rate(encoder, ctx.sample_rate);
    FLAC__stream_encoder_set_streamable_subset(encoder, false);

    if (preserve_metadata_ && metadata_blocks && num_blocks > 0) {
        FLAC__stream_encoder_set_metadata(encoder, metadata_blocks, num_blocks);
    }

    auto st = FLAC__stream_encoder_init_file(encoder, output.string().c_str(), nullptr, nullptr);
    if (st != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
        Logger::log(LogLevel::ERROR,
                    std::string("FLAC init error: ") + FLAC__StreamEncoderInitStatusString[st],
                    "flac_encoder");
        FLAC__stream_encoder_delete(encoder);
        throw std::runtime_error("FLAC init error: " + std::string(FLAC__StreamEncoderInitStatusString[st]));
    }

    if (!FLAC__stream_encoder_process_interleaved(
        encoder,
        ctx.pcm_data.data(),
        ctx.pcm_data.size() / ctx.channels)) {
        Logger::log(LogLevel::ERROR, "Error during FLAC file write", "flac_encoder");
        FLAC__stream_encoder_finish(encoder);
        FLAC__stream_encoder_delete(encoder);
        throw std::runtime_error("Error during FLAC file write");
    }

    FLAC__stream_encoder_finish(encoder);
    FLAC__stream_encoder_delete(encoder);

    // cleanup metadata
    if (metadata_blocks) {
        for (unsigned i = 0; i < num_blocks; ++i) {
            FLAC__metadata_object_delete(metadata_blocks[i]);
        }
        free(metadata_blocks);
    }

    Logger::log(LogLevel::INFO, "FLAC reencoding completed: " + output.string(), "flac_encoder");
    return true;
}
