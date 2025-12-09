//
// Created by Giuseppe Francione on 19/10/25.
//

#include "../../include/flac_processor.hpp"
#include "../../include/logger.hpp"
#include <FLAC/all.h>
#include <stdexcept>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <taglib/tag.h>
#include "audio_metadata_util.hpp"
#include "file_type.hpp"
#include "file_utils.hpp"
#include "random_utils.hpp"

namespace chisel {

struct TranscodeContext {
    FLAC__StreamEncoder* encoder = nullptr;
    std::filesystem::path output;
    bool encoder_initialized = false;
    bool preserve_metadata = true;
    FLAC__StreamMetadata** metadata_blocks = nullptr;
    unsigned num_blocks = 0;
    bool failed = false;
};

/**
 * @brief FLAC decoder write callback.
 * This function is called by the decoder for each decoded audio frame. It passes
 * the decoded audio to the encoder, initializing it on the first frame.
 * @param frame The decoded FLAC frame.
 * @param buffer An array of pointers to the decoded audio samples.
 * @param client_data A pointer to the TranscodeContext.
 * @return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE on success, or ABORT on failure.
 */
static FLAC__StreamDecoderWriteStatus write_callback(
    const FLAC__StreamDecoder*,
    const FLAC__Frame* frame,
    const FLAC__int32* const buffer[],
    void* client_data)
{
    auto* ctx = static_cast<TranscodeContext*>(client_data);
    if (!ctx->encoder_initialized) {
        FLAC__stream_encoder_set_channels(ctx->encoder, frame->header.channels);
        FLAC__stream_encoder_set_bits_per_sample(ctx->encoder, frame->header.bits_per_sample);
        FLAC__stream_encoder_set_sample_rate(ctx->encoder, frame->header.sample_rate);
        FLAC__stream_encoder_set_compression_level(ctx->encoder, 8);
        FLAC__stream_encoder_set_blocksize(ctx->encoder, 0);
        FLAC__stream_encoder_set_do_mid_side_stereo(ctx->encoder, true);
        FLAC__stream_encoder_set_loose_mid_side_stereo(ctx->encoder, false);
        FLAC__stream_encoder_set_apodization(ctx->encoder, "tukey(0.5);partial_tukey(2);punchout_tukey(3);gauss(0.2)");
        FLAC__stream_encoder_set_max_lpc_order(ctx->encoder, 16);
        FLAC__stream_encoder_set_min_residual_partition_order(ctx->encoder, 0);
        FLAC__stream_encoder_set_max_residual_partition_order(ctx->encoder, 6);
        FLAC__stream_encoder_set_do_exhaustive_model_search(ctx->encoder, true);
        FLAC__stream_encoder_set_streamable_subset(ctx->encoder, false);

        if (ctx->preserve_metadata && ctx->metadata_blocks && ctx->num_blocks > 0) {
            FLAC__stream_encoder_set_metadata(ctx->encoder, ctx->metadata_blocks, ctx->num_blocks);
        }

        const auto st = FLAC__stream_encoder_init_file(
            ctx->encoder, ctx->output.string().c_str(), nullptr, nullptr);
        if (st != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
            Logger::log(LogLevel::Error,
                        std::string("FLAC init error: ") + FLAC__StreamEncoderInitStatusString[st],
                        "flac_processor");
            ctx->failed = true;
            return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
        }
        ctx->encoder_initialized = true;
        Logger::log(LogLevel::Debug, "encoder initialized", "flac_processor");
    }

    if (!FLAC__stream_encoder_process(ctx->encoder, buffer, frame->header.blocksize)) {
        Logger::log(LogLevel::Error, "encoder process failed", "flac_processor");
        ctx->failed = true;
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

/**
 * @brief FLAC decoder metadata callback (stub).
 * This function is called for each metadata block but does nothing in this implementation.
 */
static void metadata_callback(const FLAC__StreamDecoder*, const FLAC__StreamMetadata*, void*) {}

/**
 * @brief FLAC decoder error callback.
 * This function is called when a decoding error occurs.
 * @param status The specific error status code.
 */
static void error_callback(const FLAC__StreamDecoder*, FLAC__StreamDecoderErrorStatus status, void*) {
    Logger::log(LogLevel::Warning,
                std::string("FLAC decoder: ") + FLAC__StreamDecoderErrorStatusString[status],
                "libFLAC");
}

void FlacProcessor::recompress(const std::filesystem::path& input,
                               const std::filesystem::path& output,
                               const bool preserve_metadata)
{
    Logger::log(LogLevel::Info, "Starting FLAC re-encoding: " + input.string(), "flac_processor");

    if (std::filesystem::exists(output)) {
        std::filesystem::remove(output);
    }

    TranscodeContext ctx;
    ctx.output = output;
    ctx.preserve_metadata = preserve_metadata;

    // metadata copy (optional)
    FLAC__Metadata_Chain* chain = nullptr;
    FLAC__Metadata_Iterator* it = nullptr;
    if (ctx.preserve_metadata) {
        chain = FLAC__metadata_chain_new();
        if (chain && FLAC__metadata_chain_read(chain, input.string().c_str())) {
            it = FLAC__metadata_iterator_new();
            FLAC__metadata_iterator_init(it, chain);
            unsigned count = 0;
            do {
                const FLAC__StreamMetadata* block = FLAC__metadata_iterator_get_block(it);
                // skip streaminfo (handled by encoder) and picture (handled by finalize_extraction)
                if (block &&
                    block->type != FLAC__METADATA_TYPE_STREAMINFO &&
                    block->type != FLAC__METADATA_TYPE_PICTURE) {
                    ++count;
                }
            } while (FLAC__metadata_iterator_next(it));

            if (count > 0) {
                ctx.metadata_blocks = static_cast<FLAC__StreamMetadata**>(
                    std::malloc(sizeof(FLAC__StreamMetadata*) * count));
                ctx.num_blocks = count;
                FLAC__metadata_iterator_init(it, chain);
                unsigned idx = 0;
                do {
                    const FLAC__StreamMetadata* block = FLAC__metadata_iterator_get_block(it);
                    if (block &&
                        block->type != FLAC__METADATA_TYPE_STREAMINFO &&
                        block->type != FLAC__METADATA_TYPE_PICTURE) {
                        ctx.metadata_blocks[idx++] = FLAC__metadata_object_clone(block);
                    }
                } while (FLAC__metadata_iterator_next(it));
            }
        }
    }

    // encoder/decoder setup
    ctx.encoder = FLAC__stream_encoder_new();
    if (!ctx.encoder) {
        Logger::log(LogLevel::Error, "Can't create encoder", "flac_processor");
        if (ctx.metadata_blocks) {
            for (unsigned i = 0; i < ctx.num_blocks; ++i) FLAC__metadata_object_delete(ctx.metadata_blocks[i]);
            std::free(ctx.metadata_blocks);
        }
        if (it) FLAC__metadata_iterator_delete(it);
        if (chain) FLAC__metadata_chain_delete(chain);
        throw std::runtime_error("Can't create encoder");
    }

    FLAC__StreamDecoder* decoder = FLAC__stream_decoder_new();
    if (!decoder) {
        Logger::log(LogLevel::Error, "Can't create decoder", "flac_processor");
        FLAC__stream_encoder_delete(ctx.encoder);
        if (ctx.metadata_blocks) {
            for (unsigned i = 0; i < ctx.num_blocks; ++i) FLAC__metadata_object_delete(ctx.metadata_blocks[i]);
            std::free(ctx.metadata_blocks);
        }
        if (it) FLAC__metadata_iterator_delete(it);
        if (chain) FLAC__metadata_chain_delete(chain);
        throw std::runtime_error("Can't create decoder");
    }

    const auto ist = FLAC__stream_decoder_init_file(
        decoder, input.string().c_str(), write_callback, metadata_callback, error_callback, &ctx);
    if (ist != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
        Logger::log(LogLevel::Error, "decoder init failed", "flac_processor");
        FLAC__stream_decoder_delete(decoder);
        FLAC__stream_encoder_delete(ctx.encoder);
        if (ctx.metadata_blocks) {
            for (unsigned i = 0; i < ctx.num_blocks; ++i) FLAC__metadata_object_delete(ctx.metadata_blocks[i]);
            std::free(ctx.metadata_blocks);
        }
        if (it) FLAC__metadata_iterator_delete(it);
        if (chain) FLAC__metadata_chain_delete(chain);
        throw std::runtime_error("decoder init failed");
    }

    // transcode
    const bool ok = FLAC__stream_decoder_process_until_end_of_stream(decoder);
    const bool failed = (!ok) || ctx.failed;

    FLAC__stream_decoder_finish(decoder);
    FLAC__stream_decoder_delete(decoder);

    if (ctx.encoder_initialized) {
        FLAC__stream_encoder_finish(ctx.encoder);
    }
    FLAC__stream_encoder_delete(ctx.encoder);

    if (ctx.metadata_blocks) {
        for (unsigned i = 0; i < ctx.num_blocks; ++i) FLAC__metadata_object_delete(ctx.metadata_blocks[i]);
        std::free(ctx.metadata_blocks);
    }
    if (it) FLAC__metadata_iterator_delete(it);
    if (chain) FLAC__metadata_chain_delete(chain);

    if (failed) {
        Logger::log(LogLevel::Error, "decoding or encoding failed", "flac_processor");
        throw std::runtime_error("FLAC transcoding failed");
    }

    Logger::log(LogLevel::Info, "FLAC re-encoding completed: " + output.string(), "flac_processor");
}
/**
 * @brief (Phase 1) Extracts cover art from the FLAC file into a temp directory.
 *
 * Uses AudioMetadataUtil to extract all embedded pictures and saves
 * their metadata into an AudioExtractionState object, which is stored
 * in the ExtractedContent::extras std::any.
 *
 * @param input_path Path to the source FLAC file.
 * @return std::optional<ExtractedContent> containing extracted files and state.
 */
std::optional<ExtractedContent> FlacProcessor::prepare_extraction(
    const std::filesystem::path& input_path)
{
    Logger::log(LogLevel::Info, "FLAC: Preparing cover art extraction for: " + input_path.string(), "flac_processor");

    ExtractedContent content;
    content.original_path = input_path;
    content.temp_dir = make_temp_dir_for(input_path, "flac-processor");

    // audiometadatautil does the heavy lifting
    AudioExtractionState state = AudioMetadataUtil::extractCovers(input_path, content.temp_dir);

    if (state.extracted_covers.empty()) {
        Logger::log(LogLevel::Debug, "FLAC: No embedded cover art found.", "flac_processor");
        cleanup_temp_dir(content.temp_dir);
        return std::nullopt; // nothing to extract
    }

    // populate extracted files for ProcessorExecutor
    for (const auto& cover_info : state.extracted_covers) {
        content.extracted_files.push_back(cover_info.temp_file_path);
    }

    // save state in std::any for phase 3
    content.extras = std::make_any<AudioExtractionState>(std::move(state));

    Logger::log(LogLevel::Debug, "FLAC: Extracted " + std::to_string(content.extracted_files.size()) + " covers.", "flac_processor");
    content.format = ContainerFormat::Unknown; // You may want to define ContainerFormat::Flac
    return content;
}

/**
 * @brief (Phase 3) Rebuilds the FLAC file with optimized cover art.
 *
 * This function is called *after* recompress (Phase 2) has already
 * optimized the audio stream and other image processors have optimized
 * the extracted cover art files in the temp directory.
 *
 * @param content The ExtractedContent struct containing the state.
 * @return Path to the newly created temporary FLAC file.
 */
std::filesystem::path FlacProcessor::finalize_extraction(const ExtractedContent &content)
{
    Logger::log(LogLevel::Info, "FLAC: Finalizing (re-inserting covers) for: " + content.original_path.string(), "flac_processor");

    // 1. retrieve state from std::any
    const AudioExtractionState* state_ptr = std::any_cast<AudioExtractionState>(&content.extras);
    if (!state_ptr) {
        Logger::log(LogLevel::Error, "FLAC: Failed to retrieve extraction state (std::any_cast failed).", "flac_processor");
        cleanup_temp_dir(content.temp_dir);
        return {}; // return empty path on failure
    }

    // 2. define a new temp path for the *final* file
    const std::filesystem::path& optimized_audio_path = content.original_path; // this is the audio already optimized by recompress
    const std::filesystem::path final_temp_path = std::filesystem::temp_directory_path() /
                                                  (optimized_audio_path.stem().string() + "_final" + RandomUtils::random_suffix() + ".flac");

    try {
        // 3. copy the optimized audio (phase 2) to the final location
        std::filesystem::copy_file(optimized_audio_path, final_temp_path, std::filesystem::copy_options::overwrite_existing);
    } catch (const std::exception& e) {
        Logger::log(LogLevel::Error, "FLAC: Failed to copy audio file for finalization: " + std::string(e.what()), "flac_processor");
        cleanup_temp_dir(content.temp_dir);
        return {};
    }

    // 4. use audiometadatautil to re-insert covers (read from temp_dir) into the final file
    //    images in temp_dir were already optimized in phase 2.
    if (!AudioMetadataUtil::rebuildCovers(final_temp_path, *state_ptr)) {
        Logger::log(LogLevel::Error, "FLAC: AudioMetadataUtil::rebuildCovers failed for: " + final_temp_path.string(), "flac_processor");
        cleanup_temp_dir(content.temp_dir);
        std::filesystem::remove(final_temp_path); // remove the partially good file
        return {};
    }

    // 5. cleanup
    cleanup_temp_dir(content.temp_dir);

    // 6. return path to the finalized file.
    // processorexecutor will handle replacing the original.
    return final_temp_path;
}


std::string FlacProcessor::get_raw_checksum(const std::filesystem::path& file_path) const {
    FLAC__StreamMetadata* metadata = nullptr;

    if (!FLAC__metadata_get_streaminfo(file_path.string().c_str(), metadata)) {
        throw std::runtime_error("Failed to read STREAMINFO from FLAC file: " + file_path.string());
    }

    std::ostringstream oss;
    for (int i = 0; i < 16; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(metadata->data.stream_info.md5sum[i]);
    }

    FLAC__metadata_object_delete(metadata);
    return oss.str();
}

/**
 * @brief Decodes a FLAC file into a raw PCM audio buffer.
 * This is used for checksum verification.
 * @param file The path to the FLAC file.
 * @param sample_rate Output parameter for the sample rate.
 * @param channels Output parameter for the number of channels.
 * @param bps Output parameter for the bits per sample.
 * @return A vector of 32-bit integers representing the decoded PCM data.
 */
std::vector<int32_t> decode_flac_pcm(const std::filesystem::path& file,
                                     unsigned& sample_rate,
                                     unsigned& channels,
                                     unsigned& bps) {
    FLAC__StreamDecoder* dec = FLAC__stream_decoder_new();
    if (!dec) throw std::runtime_error("Cannot create FLAC decoder");

    std::vector<int32_t> pcm;
    struct Context {
        std::vector<int32_t>* pcm;
        unsigned channels{};
        unsigned bps{};
        bool failed{false};
    } ctx;
    ctx.pcm = &pcm;

    auto write_cb = [](const FLAC__StreamDecoder*,
                       const FLAC__Frame* frame,
                       const FLAC__int32* const buffer[],
                       void* client_data) -> FLAC__StreamDecoderWriteStatus {
        auto* c = static_cast<Context*>(client_data);
        const size_t n = frame->header.blocksize;
        for (size_t i = 0; i < n; ++i) {
            for (unsigned ch = 0; ch < frame->header.channels; ++ch) {
                c->pcm->push_back(buffer[ch][i]);
            }
        }
        return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
    };

    auto metadata_cb = [](const FLAC__StreamDecoder*,
                          const FLAC__StreamMetadata* md,
                          void* client_data) {
        if (md->type == FLAC__METADATA_TYPE_STREAMINFO) {
            auto* c = static_cast<Context*>(client_data);
            c->channels = md->data.stream_info.channels;
            c->bps      = md->data.stream_info.bits_per_sample;
        }
    };

    auto error_cb = [](const FLAC__StreamDecoder*,
                       FLAC__StreamDecoderErrorStatus,
                       void*) {};

    if (FLAC__stream_decoder_init_file(dec,
                                       file.string().c_str(),
                                       write_cb,
                                       metadata_cb,
                                       error_cb,
                                       &ctx) != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
        FLAC__stream_decoder_delete(dec);
        throw std::runtime_error("FLAC decoder init failed");
    }

    if (!FLAC__stream_decoder_process_until_end_of_stream(dec)) {
        FLAC__stream_decoder_delete(dec);
        throw std::runtime_error("FLAC decode failed");
    }

    sample_rate = FLAC__stream_decoder_get_sample_rate(dec);
    channels    = ctx.channels;
    bps         = ctx.bps;

    FLAC__stream_decoder_delete(dec);
    return pcm;
}


bool FlacProcessor::raw_equal(const std::filesystem::path& a,
                              const std::filesystem::path& b) const {
    unsigned ra, ca, bpsa;
    unsigned rb, cb, bpsb;
    const auto pcmA = decode_flac_pcm(a, ra, ca, bpsa);
    const auto pcmB = decode_flac_pcm(b, rb, cb, bpsb);

    if (ra != rb || ca != cb || bpsa != bpsb) return false;
    return pcmA == pcmB;
}

} // namespace chisel