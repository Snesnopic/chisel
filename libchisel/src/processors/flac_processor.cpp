//
// Created by Giuseppe Francione on 19/10/25.
//

#include "../../include/flac_processor.hpp"
#include "../../include/logger.hpp"
#include "../../include/random_utils.hpp"
#include <FLAC/all.h>
#include <FLAC++/metadata.h>
#include <stdexcept>
#include <cstdlib>
#include <sstream>
#include <fstream>
#include <chrono>

namespace chisel {

namespace fs = std::filesystem;

// --- helpers ---

static fs::path make_temp_dir() {
    const auto base = fs::temp_directory_path();
    const auto now  = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path dir = base / ("chisel_flac_" + std::to_string(now) + "_" + RandomUtils::random_suffix());
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

static std::string sanitize_filename(const std::string& input) {
    std::string sanitized;
    sanitized.reserve(input.length());
    for (char c : input) {
        if (c >= 0 && (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-')) {
            sanitized += c;
        }
    }
    return sanitized;
}

std::optional<ExtractedContent> FlacProcessor::prepare_extraction(const std::filesystem::path& input_path)
{
    ExtractedContent content;
    content.original_path = input_path;
    content.temp_dir = make_temp_dir();

    FLAC::Metadata::Chain chain;
    if (!chain.read(input_path.string().c_str())) {
        Logger::log(LogLevel::Error, "Failed to read FLAC metadata from: " + input_path.string(), "flac_processor");
        return std::nullopt;
    }

    FLAC::Metadata::Iterator it;
    it.init(chain);

    unsigned index = 0;
    unsigned pic_index = 0;
    std::vector<PictureMetadata> picture_metadata;

    do {
        FLAC::Metadata::Prototype* block = it.get_block();
        if (block->get_type() == FLAC__METADATA_TYPE_PICTURE) {
            auto* pic = dynamic_cast<FLAC::Metadata::Picture*>(block);
            if (pic) {
                PictureMetadata meta;
                meta.index = index;
                meta.description = (const char*)pic->get_description();
                meta.mime_type = pic->get_mime_type();
                meta.type = pic->get_type();
                meta.width = pic->get_width();
                meta.height = pic->get_height();
                meta.depth = pic->get_depth();
                meta.colors = pic->get_colors();
                picture_metadata.push_back(meta);

                std::string filename = sanitize_filename(meta.description);
                if (filename.empty()) {
                    filename = "picture_" + std::to_string(pic_index);
                    if (meta.mime_type == "image/jpeg") filename += ".jpg";
                    else if (meta.mime_type == "image/png") filename += ".png";
                }

                fs::path out_path = content.temp_dir / filename;
                std::ofstream ofs(out_path, std::ios::binary);
                ofs.write(reinterpret_cast<const char*>(pic->get_data()), pic->get_data_length());
                content.extracted_files.push_back(out_path);
                pic_index++;
            }
        }
        delete block;
        index++;
    } while (it.next());

    content.processor_context = std::make_any<std::vector<PictureMetadata>>(picture_metadata);

    return content;
}

std::filesystem::path FlacProcessor::finalize_extraction(const ExtractedContent &content, [[maybe_unused]] ContainerFormat target_format)
{
    if (!content.processor_context.has_value()) {
        return content.original_path;
    }

    const auto& picture_metadata = std::any_cast<const std::vector<PictureMetadata>&>(content.processor_context);
    if (picture_metadata.empty()) {
        return content.original_path;
    }

    std::vector<std::unique_ptr<FLAC::Metadata::Picture>> new_pictures;
    for (size_t i = 0; i < picture_metadata.size(); ++i) {
        const auto& meta = picture_metadata[i];
        auto pic = std::make_unique<FLAC::Metadata::Picture>();
        pic->set_description((const FLAC__byte*)meta.description.c_str());
        pic->set_mime_type(meta.mime_type.c_str());
        pic->set_type(meta.type);
        pic->set_width(meta.width);
        pic->set_height(meta.height);
        pic->set_depth(meta.depth);
        pic->set_colors(meta.colors);

        std::ifstream ifs(content.extracted_files[i], std::ios::binary | std::ios::ate);
        std::streamsize size = ifs.tellg();
        ifs.seekg(0, std::ios::beg);
        std::vector<char> buffer(size);
        if (ifs.read(buffer.data(), size)) {
            pic->set_data((const FLAC__byte*)buffer.data(), size);
        }
        new_pictures.push_back(std::move(pic));
    }

    fs::path temp_output_path = make_temp_dir() / content.original_path.filename();
    fs::copy_file(content.original_path, temp_output_path);

    FLAC::Metadata::Chain chain;
    if (!chain.read(temp_output_path.string().c_str())) {
        Logger::log(LogLevel::Error, "Failed to read FLAC metadata from: " + temp_output_path.string(), "flac_processor");
        return {};
    }

    // Create a new chain with the updated metadata
    FLAC::Metadata::Chain new_chain;
    FLAC::Metadata::Iterator it;
    it.init(chain);

    std::vector<std::unique_ptr<FLAC::Metadata::Prototype>> new_blocks;
    do {
        FLAC::Metadata::Prototype* block = it.get_block();
        if (block->get_type() != FLAC__METADATA_TYPE_PICTURE) {
            new_blocks.push_back(std::unique_ptr<FLAC::Metadata::Prototype>(block->clone()));
        }
        delete block;
    } while (it.next());

    // Insert new pictures in correct order
    for (size_t i = 0; i < new_pictures.size(); ++i) {
        new_blocks.insert(new_blocks.begin() + picture_metadata[i].index, std::move(new_pictures[i]));
    }

    for (auto& block : new_blocks) {
        new_chain.push_back(block.release());
    }

    if (!new_chain.write(temp_output_path.string().c_str())) {
        Logger::log(LogLevel::Error, "Failed to write updated metadata to: " + temp_output_path.string(), "flac_processor");
        return {};
    }

    return temp_output_path;
}

struct TranscodeContext {
    FLAC__StreamEncoder* encoder = nullptr;
    std::filesystem::path output;
    bool encoder_initialized = false;
    bool preserve_metadata = true;
    FLAC__StreamMetadata** metadata_blocks = nullptr;
    unsigned num_blocks = 0;
    bool failed = false;
};

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

static void metadata_callback(const FLAC__StreamDecoder*, const FLAC__StreamMetadata*, void*) {}
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

    // --- metadata copy (optional) ---
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
                if (block && block->type != FLAC__METADATA_TYPE_STREAMINFO) ++count;
            } while (FLAC__metadata_iterator_next(it));
            if (count > 0) {
                ctx.metadata_blocks = static_cast<FLAC__StreamMetadata**>(
                    std::malloc(sizeof(FLAC__StreamMetadata*) * count));
                ctx.num_blocks = count;
                FLAC__metadata_iterator_init(it, chain);
                unsigned idx = 0;
                do {
                    const FLAC__StreamMetadata* block = FLAC__metadata_iterator_get_block(it);
                    if (block && block->type != FLAC__METADATA_TYPE_STREAMINFO) {
                        ctx.metadata_blocks[idx++] = FLAC__metadata_object_clone(block);
                    }
                } while (FLAC__metadata_iterator_next(it));
            }
        }
    }

    // --- encoder/decoder setup ---
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

    // --- transcode ---
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