//
// Created by Giuseppe Francione on 10/08/26.
//

#include "../../include/flacout_processor.hpp"
#include "../../include/logger.hpp"
#include "../../include/audio_metadata_util.hpp"
#include "../../include/random_utils.hpp"
#include "../../include/thread_pool.hpp"
#include <flacoutcpp.hpp>
#include <FLAC/all.h>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace chisel {

// defined in flac_processor.cpp, reused here instead of duplicating the decode loop
std::vector<int32_t> decode_flac_pcm(const std::filesystem::path& file,
                                     unsigned& sample_rate,
                                     unsigned& channels,
                                     unsigned& bps);

namespace {

/**
 * @brief Why flacoutcpp refuses @p file because of the file itself, if it does:
 * what `flac --test` rejects (a decoder error, a sample count other than
 * STREAMINFO's, an MD5 mismatch) or a STREAMINFO without a sample count.
 */
std::optional<std::string> input_problem(const std::filesystem::path& file) {
    struct Check {
        std::ifstream in;
        unsigned channels = 0;
        FLAC__uint64 declared = 0;
        FLAC__uint64 decoded = 0;
        std::string problem;
    } check;
    // libFLAC skips ID3v2 tags but not the zero padding some taggers put after them
    check.in.open(file, std::ios::binary);
    check.in.seekg(static_cast<std::streamoff>(AudioMetadataUtil::foreignId3v2Tags(file).size()));
    if (!check.in) return std::nullopt;

    FLAC__StreamDecoder* dec = FLAC__stream_decoder_new();
    if (!dec) return std::nullopt;
    FLAC__stream_decoder_set_md5_checking(dec, true);

    const auto read = [](const FLAC__StreamDecoder*, FLAC__byte buffer[], size_t* bytes,
                         void* data) -> FLAC__StreamDecoderReadStatus {
        auto& c = *static_cast<Check*>(data);
        c.in.read(reinterpret_cast<char*>(buffer), static_cast<std::streamsize>(*bytes));
        *bytes = static_cast<size_t>(c.in.gcount());
        return *bytes == 0 ? FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM : FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
    };
    const auto write = [](const FLAC__StreamDecoder*, const FLAC__Frame* frame, const FLAC__int32* const[],
                          void* data) -> FLAC__StreamDecoderWriteStatus {
        auto& c = *static_cast<Check*>(data);
        if (frame->header.channels != c.channels) {
            c.problem = "a frame with " + std::to_string(frame->header.channels) + " channels, STREAMINFO declares "
                        + std::to_string(c.channels);
            return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
        }
        c.decoded += frame->header.blocksize;
        return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
    };
    const auto metadata = [](const FLAC__StreamDecoder*, const FLAC__StreamMetadata* md, void* data) {
        if (md->type != FLAC__METADATA_TYPE_STREAMINFO) return;
        auto& c = *static_cast<Check*>(data);
        c.channels = md->data.stream_info.channels;
        c.declared = md->data.stream_info.total_samples;
    };
    const auto error = [](const FLAC__StreamDecoder*, const FLAC__StreamDecoderErrorStatus status, void* data) {
        auto& c = *static_cast<Check*>(data);
        if (c.problem.empty()) c.problem = std::string("decoder error ") + FLAC__StreamDecoderErrorStatusString[status];
    };

    if (FLAC__stream_decoder_init_stream(dec, read, nullptr, nullptr, nullptr, nullptr, write, metadata, error, &check)
        != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
        FLAC__stream_decoder_delete(dec);
        return std::nullopt;
    }
    const bool decoded = FLAC__stream_decoder_process_until_end_of_stream(dec);
    const FLAC__StreamDecoderState state = FLAC__stream_decoder_get_state(dec);
    // false on an MD5 mismatch; libFLAC doesn't check an MD5 left unset
    const bool md5_ok = FLAC__stream_decoder_finish(dec);
    FLAC__stream_decoder_delete(dec);

    const auto damaged = [](const std::string& why) { return "Damaged FLAC data (" + why + ")"; };
    if (!check.problem.empty()) return damaged(check.problem);
    if (!decoded) return damaged(std::string("decoding stopped in state ") + FLAC__StreamDecoderStateString[state]);
    if (check.declared == 0) return std::string("No sample count in STREAMINFO");
    if (check.decoded != check.declared) {
        return damaged(std::to_string(check.decoded) + " samples, STREAMINFO declares " + std::to_string(check.declared));
    }
    if (!md5_ok) return damaged("audio doesn't match the MD5 in STREAMINFO");
    return std::nullopt;
}

} // namespace

void FlacoutProcessor::recompress(const std::filesystem::path &input,
                                  const std::filesystem::path &output, const ProcessingOptions &options) {
    Logger::log(LogLevel::Debug, "Entering recompress for " + input.string(), get_name());

    flacoutcpp::Config cfg;
    cfg.copy_metadata = options.preserve_metadata;
    cfg.verbose = false;
    if (options.flac_exhaustive) {
        // what flacout's own -e means given alone: exact pricing, the full predictor sweep, its own window set
        cfg.exhaustive = true;
        cfg.max_candidates = 0;
        cfg.precision_rungs = 0;
        cfg.adaptive_windows = false;
    }
    // only ever called on this thread
    std::vector<std::string> errors;
    cfg.on_message = [&](const flacoutcpp::MessageLevel level, const std::string_view text) {
        if (level == flacoutcpp::MessageLevel::error) errors.emplace_back(text);
        Logger::log(level == flacoutcpp::MessageLevel::warning ? LogLevel::Warning : LogLevel::Debug,
                    std::string(text), get_name());
    };
    // flacoutcpp would otherwise start a thread per core for every file, whatever --threads says
    cfg.max_threads = 1;
    if (ThreadPool* pool = ThreadPool::current()) {
        cfg.max_threads = pool->size();
        cfg.parallel_for = [pool](const std::size_t n, const std::function<void(std::size_t)>& body) {
            pool->parallel_for(n, body);
        };
    }

    if (!flacoutcpp::optimise(input.string(), output.string(), cfg)) {
        // flacoutcpp refuses what flac --test rejects, rather than write libFLAC's guesses for the damaged frames
        if (const auto problem = input_problem(input)) {
            Logger::log(LogLevel::Warning, *problem + ", left as is: " + input.string(), get_name());
            std::filesystem::copy_file(input, output, std::filesystem::copy_options::overwrite_existing);
            return;
        }
        const std::string cause = errors.empty() ? std::string("optimise failed") : errors.back();
        Logger::log(LogLevel::Error, "flacoutcpp: " + cause, get_name());
        throw std::runtime_error("flacoutcpp: " + cause);
    }

    Logger::log(LogLevel::Debug, "Exiting recompress for " + output.string(), get_name());
}

std::optional<ExtractedContent> FlacoutProcessor::prepare_extraction(const std::filesystem::path& input_path) {
    return AudioMetadataUtil::prepareCoverExtraction(input_path, "flacout-processor", get_name());
}

std::filesystem::path FlacoutProcessor::finalize_extraction(const ExtractedContent &content, const ProcessingOptions &options) {
    return AudioMetadataUtil::finalizeCoverExtraction(content, get_name());
}

std::string FlacoutProcessor::get_raw_checksum(const std::filesystem::path& file_path) const {
    // FLAC__StreamMetadata has no internal allocation: pass the address of a
    // plain struct, not a pointer-to-pointer (there is nothing to free after)
    FLAC__StreamMetadata metadata;

    if (!FLAC__metadata_get_streaminfo(file_path.string().c_str(), &metadata)) {
        throw std::runtime_error("Failed to read STREAMINFO from FLAC file: " + file_path.string());
    }

    std::ostringstream oss;
    for (int i = 0; i < 16; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(metadata.data.stream_info.md5sum[i]);
    }

    return oss.str();
}

bool FlacoutProcessor::raw_equal(const std::filesystem::path& a,
                                 const std::filesystem::path& b) const {
    // libFLAC doesn't find the stream behind ID3v2 tags followed by zero padding: decode it without them
    const auto decode = [](const std::filesystem::path& file, unsigned& rate, unsigned& channels, unsigned& bps) {
        const std::vector<uint8_t> id3 = AudioMetadataUtil::foreignId3v2Tags(file);
        if (id3.empty()) return decode_flac_pcm(file, rate, channels, bps);
        const std::filesystem::path stripped =
            std::filesystem::temp_directory_path() / ("flacout_raw_" + RandomUtils::random_suffix() + ".flac");
        AudioMetadataUtil::writeWithHead(file, stripped, {}, id3.size());
        std::vector<int32_t> pcm;
        try {
            pcm = decode_flac_pcm(stripped, rate, channels, bps);
        } catch (...) {
            std::error_code ec;
            std::filesystem::remove(stripped, ec);
            throw;
        }
        std::error_code ec;
        std::filesystem::remove(stripped, ec);
        return pcm;
    };
    unsigned ra, ca, bpsa;
    unsigned rb, cb, bpsb;
    const auto pcmA = decode(a, ra, ca, bpsa);
    const auto pcmB = decode(b, rb, cb, bpsb);

    if (ra != rb || ca != cb || bpsa != bpsb) return false;
    return pcmA == pcmB;
}

} // namespace chisel
