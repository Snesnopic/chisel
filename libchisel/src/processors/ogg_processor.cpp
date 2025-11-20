//
// Created by Giuseppe Francione on 18/11/25.
//

#include "../../include/ogg_processor.hpp"
#include "../../include/logger.hpp"
#include "../../include/audio_metadata_util.hpp"
#include "../../include/file_utils.hpp"
#include "../../include/random_utils.hpp"
#include <stdexcept>
#include <filesystem>
#include <cstdio>
#include <vector>
#include <FLAC/all.h>
#include "file_type.hpp"

namespace chisel {
namespace fs = std::filesystem;

static const char* processor_tag() {
    return "OggProcessor";
}

namespace {

    // file* wrappers for unicode path support on windows

    FLAC__StreamDecoderReadStatus read_cb(const FLAC__StreamDecoder*, FLAC__byte buffer[], size_t *bytes, void *client_data) {
        FILE* f = static_cast<FILE*>(client_data);
        if (*bytes > 0) {
            *bytes = fread(buffer, sizeof(FLAC__byte), *bytes, f);
            if (*bytes == 0 && ferror(f)) return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
            if (*bytes == 0 && feof(f)) return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
            return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
        }
        return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
    }

    FLAC__StreamDecoderSeekStatus seek_cb(const FLAC__StreamDecoder*, FLAC__uint64 absolute_byte_offset, void *client_data) {
        FILE* f = static_cast<FILE*>(client_data);
        if (fseek(f, (long)absolute_byte_offset, SEEK_SET) < 0) return FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;
        return FLAC__STREAM_DECODER_SEEK_STATUS_OK;
    }

    FLAC__StreamDecoderTellStatus tell_cb(const FLAC__StreamDecoder*, FLAC__uint64 *absolute_byte_offset, void *client_data) {
        const auto f = static_cast<FILE*>(client_data);
        const long pos = ftell(f);
        if (pos < 0) return FLAC__STREAM_DECODER_TELL_STATUS_ERROR;
        *absolute_byte_offset = static_cast<FLAC__uint64>(pos);
        return FLAC__STREAM_DECODER_TELL_STATUS_OK;
    }

    FLAC__StreamDecoderLengthStatus length_cb(const FLAC__StreamDecoder*, FLAC__uint64 *stream_length, void *client_data) {
        const auto f = static_cast<FILE*>(client_data);
        const long curr = ftell(f);
        fseek(f, 0, SEEK_END);
        const long len = ftell(f);
        fseek(f, curr, SEEK_SET);
        if (len < 0) return FLAC__STREAM_DECODER_LENGTH_STATUS_ERROR;
        *stream_length = static_cast<FLAC__uint64>(len);
        return FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
    }

    FLAC__bool eof_cb(const FLAC__StreamDecoder*, void *client_data) {
        const auto f = static_cast<FILE*>(client_data);
        return feof(f) ? true : false;
    }

    FLAC__StreamEncoderWriteStatus write_cb(const FLAC__StreamEncoder*, const FLAC__byte buffer[], size_t bytes, unsigned, unsigned, void *client_data) {
        const auto f = static_cast<FILE*>(client_data);
        if (fwrite(buffer, 1, bytes, f) != bytes) return FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
        return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
    }

    FLAC__StreamEncoderSeekStatus enc_seek_cb(const FLAC__StreamEncoder*, FLAC__uint64 absolute_byte_offset, void *client_data) {
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

    // context to share state between decoder and encoder callbacks
    struct TranscodeContext {
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

    // metadata callback: accumulates tags to preserve
    void dec_metadata_cb(const FLAC__StreamDecoder*, const FLAC__StreamMetadata* metadata, void* client_data) {
        auto* ctx = static_cast<TranscodeContext*>(client_data);
        if (!ctx->preserve_metadata) return;

        // preserve vorbis comments (tags)
        // pictures are handled separately by finalize_extraction, so we skip them here to avoid duplication/conflicts
        if (metadata->type == FLAC__METADATA_TYPE_VORBIS_COMMENT) {
            FLAC__StreamMetadata* copy = FLAC__metadata_object_clone(metadata);
            if (copy) {
                ctx->meta_blocks.push_back(copy);
            }
        }
    }

    // error callback
    void dec_error_cb(const FLAC__StreamDecoder*, FLAC__StreamDecoderErrorStatus status, void*) {
        Logger::log(LogLevel::Warning, "libflac warning: " + std::string(FLAC__StreamDecoderErrorStatusString[status]), processor_tag());
    }

    // decoder write callback: lazy-inits encoder and feeds audio
    FLAC__StreamDecoderWriteStatus dec_write_cb(const FLAC__StreamDecoder*, const FLAC__Frame* frame, const FLAC__int32* const buffer[], void* client_data) {
        auto* ctx = static_cast<TranscodeContext*>(client_data);

        if (!ctx->encoder_init) {
            // configure encoder parameters from first frame
            FLAC__stream_encoder_set_channels(ctx->encoder, frame->header.channels);
            FLAC__stream_encoder_set_bits_per_sample(ctx->encoder, frame->header.bits_per_sample);
            FLAC__stream_encoder_set_sample_rate(ctx->encoder, frame->header.sample_rate);

            // max compression
            FLAC__stream_encoder_set_compression_level(ctx->encoder, 8);
            FLAC__stream_encoder_set_blocksize(ctx->encoder, 0);
            FLAC__stream_encoder_set_do_exhaustive_model_search(ctx->encoder, true);
            FLAC__stream_encoder_set_min_residual_partition_order(ctx->encoder, 0);
            FLAC__stream_encoder_set_max_residual_partition_order(ctx->encoder, 6);
            FLAC__stream_encoder_set_max_lpc_order(ctx->encoder, 16);
            FLAC__stream_encoder_set_apodization(ctx->encoder, "tukey(0.5);partial_tukey(2);punchout_tukey(3);gauss(0.2)");
            FLAC__stream_encoder_set_do_mid_side_stereo(ctx->encoder, true);
            FLAC__stream_encoder_set_loose_mid_side_stereo(ctx->encoder, false);

            // set metadata before init
            if (!ctx->meta_blocks.empty()) {
                FLAC__stream_encoder_set_metadata(ctx->encoder, ctx->meta_blocks.data(), static_cast<unsigned>(ctx->meta_blocks.size()));
            }

            // init encoder in ogg mode
            if (FLAC__stream_encoder_init_ogg_stream(
                    ctx->encoder,
                    nullptr, write_cb, enc_seek_cb, enc_tell_cb, nullptr,
                    ctx->f_out) != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
                Logger::log(LogLevel::Error, "Failed to init FLAC Ogg encoder", processor_tag());
                ctx->failed = true;
                return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
            }
            ctx->encoder_init = true;
        }

        if (!FLAC__stream_encoder_process(ctx->encoder, buffer, frame->header.blocksize)) {
            Logger::log(LogLevel::Error, "Encoding process failed", processor_tag());
            ctx->failed = true;
            return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
        }

        return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
    }

} // namespace

void OggProcessor::recompress(const fs::path& input,
                              const fs::path& output,
                              bool preserve_metadata) {
    Logger::log(LogLevel::Info, "Analyzing Ogg stream: " + input.string(), processor_tag());

    FILE* f_in = chisel::open_file(input, "rb");
    if (!f_in) throw std::runtime_error("OggProcessor: cannot open input");

    FLAC__StreamDecoder* decoder = FLAC__stream_decoder_new();
    if (!decoder) {
        fclose(f_in);
        throw std::runtime_error("OggProcessor: failed to create decoder");
    }

    FLAC__StreamEncoder* encoder = FLAC__stream_encoder_new();
    if (!encoder) {
        FLAC__stream_decoder_delete(decoder);
        fclose(f_in);
        throw std::runtime_error("OggProcessor: failed to create encoder");
    }

    // prepare context
    TranscodeContext ctx;
    ctx.encoder = encoder;
    ctx.preserve_metadata = preserve_metadata;

    // attempt to init decoder as ogg-flac
    // note: we don't pass f_out yet, it's opened only if format is confirmed
    FLAC__StreamDecoderInitStatus init_stat = FLAC__stream_decoder_init_ogg_stream(
        decoder,
        read_cb, seek_cb, tell_cb, length_cb, eof_cb,
        dec_write_cb, dec_metadata_cb, dec_error_cb,
        &ctx
    );

    bool is_flac_ogg = (init_stat == FLAC__STREAM_DECODER_INIT_STATUS_OK);

    if (!is_flac_ogg) {
        // cleanup and fallback for non-flac ogg (vorbis, opus)
        FLAC__stream_decoder_delete(decoder);
        FLAC__stream_encoder_delete(encoder);
        fclose(f_in);

        Logger::log(LogLevel::Info, "Stream is not Ogg-FLAC (likely Vorbis/Opus), skipping recompression", processor_tag());

        std::error_code ec;
        fs::copy_file(input, output, fs::copy_options::overwrite_existing, ec);
        if (ec) throw std::runtime_error("Fallback copy failed: " + ec.message());
        return;
    }

    // it's ogg-flac, proceed with recompression
    FILE* f_out = chisel::open_file(output, "wb");
    if (!f_out) {
        FLAC__stream_decoder_delete(decoder);
        FLAC__stream_encoder_delete(encoder);
        fclose(f_in);
        throw std::runtime_error("OggProcessor: cannot open output");
    }
    ctx.f_out = f_out;

    Logger::log(LogLevel::Info, "Recompressing Ogg-FLAC stream...", processor_tag());
    bool success = FLAC__stream_decoder_process_until_end_of_stream(decoder);

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

    Logger::log(LogLevel::Info, "Ogg-FLAC recompression completed", processor_tag());
}

std::optional<ExtractedContent> OggProcessor::prepare_extraction(const fs::path& input_path) {
    Logger::log(LogLevel::Info, "OGG: Preparing cover art extraction for: " + input_path.string(), processor_tag());

    ExtractedContent content;
    content.original_path = input_path;
    content.temp_dir = make_temp_dir_for(input_path, "ogg-processor");

    AudioExtractionState state = AudioMetadataUtil::extractCovers(input_path, content.temp_dir);

    if (state.extracted_covers.empty()) {
        Logger::log(LogLevel::Debug, "OGG: No embedded cover art found", processor_tag());
        cleanup_temp_dir(content.temp_dir, processor_tag());
        return std::nullopt;
    }

    for (const auto& cover_info : state.extracted_covers) {
        content.extracted_files.push_back(cover_info.temp_file_path);
    }

    content.extras = std::make_any<AudioExtractionState>(std::move(state));
    content.format = ContainerFormat::Unknown;
    return content;
}

std::filesystem::path OggProcessor::finalize_extraction(const ExtractedContent &content,
                                                        ContainerFormat /*target_format*/) {
    Logger::log(LogLevel::Info, "OGG: Finalizing (re-inserting covers) for: " + content.original_path.string(), processor_tag());

    const AudioExtractionState* state_ptr = std::any_cast<AudioExtractionState>(&content.extras);
    if (!state_ptr) {
        Logger::log(LogLevel::Error, "OGG: Failed to retrieve extraction state", processor_tag());
        cleanup_temp_dir(content.temp_dir, processor_tag());
        return {};
    }

    const fs::path final_temp_path = fs::temp_directory_path() /
                                     (content.original_path.stem().string() + "_final" + RandomUtils::random_suffix() + content.original_path.extension().string());

    try {
        fs::copy_file(content.original_path, final_temp_path, fs::copy_options::overwrite_existing);
    } catch (const std::exception& e) {
        Logger::log(LogLevel::Error, "OGG: Failed to copy audio file: " + std::string(e.what()), processor_tag());
        cleanup_temp_dir(content.temp_dir, processor_tag());
        return {};
    }

    // rebuild covers using taglib (preserves other tags, modifies file in-place)
    if (!AudioMetadataUtil::rebuildCovers(final_temp_path, *state_ptr)) {
        Logger::log(LogLevel::Error, "OGG: rebuildCovers failed", processor_tag());
        cleanup_temp_dir(content.temp_dir, processor_tag());
        fs::remove(final_temp_path);
        return {};
    }

    cleanup_temp_dir(content.temp_dir, processor_tag());
    return final_temp_path;
}

} // namespace chisel