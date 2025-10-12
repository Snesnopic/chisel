//
// Created by Giuseppe Francione on 09/10/25.
//

#include "ape_encoder.hpp"
#include "../utils/logger.hpp"

#include <MACLib.h>
#include <APETag.h>
#include <vector>
#include <stdexcept>
#include <filesystem>
#include <algorithm>
#include <cstring>

// small context to hold decoded pcm
struct ApeDecodeContext {
    std::vector<uint8_t> pcm_bytes; // interleaved raw pcm bytes
    unsigned sample_rate = 0;
    unsigned channels = 0;
    unsigned bits_per_sample = 0;
    int64_t total_frames = 0;
    int64_t total_samples = 0;
};

// helper: copy apev2 metadata from input to output using CAPETag
static bool copy_apetag(const std::filesystem::path &input,
                        const std::filesystem::path &output) {
    try {
        APE::CAPETag inTag(input.wstring().c_str(), true);
        if (!inTag.GetAnalyzed())
            return false;

        APE::CAPETag outTag(output.wstring().c_str(), true);
        outTag.ClearFields();
        outTag.SetIgnoreReadOnly(true);

        for (int i = 0;; ++i) {
            APE::CAPETagField *field = inTag.GetTagField(i);
            if (!field) break;

            const APE::str_utfn *name = field->GetFieldName();
            const char *value = field->GetFieldValue();
            const int valueSize = field->GetFieldValueSize();
            const int flags = field->GetFieldFlags();
            if (!name || valueSize <= 0) continue;

            const bool isBinary = (flags & TAG_FIELD_FLAG_DATA_TYPE_BINARY) != 0;

            if (isBinary) {
                outTag.SetFieldBinary(name, value, valueSize, flags);
            } else {
                outTag.SetFieldString(name, value, true, nullptr);
            }
        }

        return outTag.Save();
    } catch (...) {
        return false;
    }
}

ApeEncoder::ApeEncoder(const bool preserve_metadata) {
    preserve_metadata_ = preserve_metadata;
}

bool ApeEncoder::recompress(const std::filesystem::path &input,
                            const std::filesystem::path &output) {
    Logger::log(LogLevel::Info, "STARTING APE RE-ENCODING: " + input.string(), "ape_encoder");

    if (std::filesystem::exists(output)) {
        std::filesystem::remove(output);
    }

    ApeDecodeContext ctx;
    int err = 0;
    APE::IAPEDecompress *pDecompress =
        CreateIAPEDecompress(input.wstring().c_str(),
                             &err,
                             true,   // read-only
                             true,   // analyze tag now
                             false); // don't read whole file at once
    if (!pDecompress || err != ERROR_SUCCESS) {
        Logger::log(LogLevel::Error, "CAN'T CREATE APE DECOMPRESS", "ape_encoder");
        if (pDecompress) delete pDecompress;
        throw std::runtime_error("Can't create APE decompress");
    }

    ctx.sample_rate     = static_cast<unsigned>(pDecompress->GetInfo(APE::IAPEDecompress::APE_INFO_SAMPLE_RATE));
    ctx.channels        = static_cast<unsigned>(pDecompress->GetInfo(APE::IAPEDecompress::APE_INFO_CHANNELS));
    ctx.bits_per_sample = static_cast<unsigned>(pDecompress->GetInfo(APE::IAPEDecompress::APE_INFO_BITS_PER_SAMPLE));
    ctx.total_frames    = pDecompress->GetInfo(APE::IAPEDecompress::APE_INFO_TOTAL_FRAMES);
    ctx.total_samples   = pDecompress->GetInfo(APE::IAPEDecompress::APE_INFO_TOTAL_BLOCKS) / ctx.channels;

    if (ctx.channels == 0 || ctx.sample_rate == 0 || ctx.bits_per_sample == 0) {
        Logger::log(LogLevel::Error, "INVALID APE FILE PARAMETERS", "ape_encoder");
        delete pDecompress;
        throw std::runtime_error("Invalid APE file");
    }

    Logger::log(LogLevel::Debug,
                "PARAMS: " + std::to_string(ctx.channels) + "ch, " +
                std::to_string(ctx.sample_rate) + "Hz, " +
                std::to_string(ctx.bits_per_sample) + "bit",
                "ape_encoder");

    const int bytes_per_sample = static_cast<int>(ctx.bits_per_sample / 8);
    constexpr int block_samples = 16384;
    const int block_bytes = block_samples * static_cast<int>(ctx.channels) * bytes_per_sample;

    std::vector<uint8_t> block(static_cast<size_t>(block_bytes));
    int64_t samples_read_total = 0;

    while (samples_read_total < ctx.total_samples) {
        const int samples_to_read = static_cast<int>(std::min<int64_t>(block_samples, ctx.total_samples - samples_read_total));
        const int bytes_to_read = samples_to_read * static_cast<int>(ctx.channels) * bytes_per_sample;

        int rc = pDecompress->GetData(block.data(), samples_to_read, nullptr);
        if (rc != ERROR_SUCCESS) {
            Logger::log(LogLevel::Error, "APE DECODING BLOCK FAILED", "ape_encoder");
            delete pDecompress;
            throw std::runtime_error("APE decoding failed");
        }

        ctx.pcm_bytes.insert(ctx.pcm_bytes.end(), block.begin(), block.begin() + bytes_to_read);
        samples_read_total += samples_to_read;
    }

    delete pDecompress;

    APE::WAVEFORMATEX wfeAudioFormat{};
    FillWaveFormatEx(&wfeAudioFormat, WAVE_FORMAT_PCM,
                     static_cast<APE::int32>(ctx.sample_rate),
                     static_cast<APE::WORD>(ctx.bits_per_sample),
                     static_cast<APE::WORD>(ctx.channels));

    APE::IAPECompress *pCompress = CreateIAPECompress();
    if (!pCompress) {
        Logger::log(LogLevel::Error, "CAN'T CREATE APE ENCODER", "ape_encoder");
        throw std::runtime_error("Can't create APE encoder");
    }

    constexpr int level = APE_COMPRESSION_LEVEL_INSANE;
    const auto maxAudioBytes = static_cast<APE::int64>(ctx.pcm_bytes.size());

    int nRetVal = pCompress->Start(
        output.wstring().c_str(),
        &wfeAudioFormat,
        false,
        maxAudioBytes,
        level,
        NULL,
        CREATE_WAV_HEADER_ON_DECOMPRESSION
    );

    if (nRetVal != 0) {
        Logger::log(LogLevel::Error, "ERROR STARTING APE ENCODER", "ape_encoder");
        APE_SAFE_DELETE(pCompress)
        throw std::runtime_error("APE encoder start failed");
    }

    const size_t chunk = 1 << 16;
    size_t offset = 0;
    while (offset < ctx.pcm_bytes.size()) {
        const size_t n = std::min(chunk, ctx.pcm_bytes.size() - offset);
        const auto add_rc = pCompress->AddData(
            ctx.pcm_bytes.data() + offset,
            static_cast<APE::int64>(n)
        );
        if (add_rc != 0) {
            Logger::log(LogLevel::Error, "APE ENCODER ADDDATA() FAILED", "ape_encoder");
            pCompress->Finish(NULL, 0, 0);
            APE_SAFE_DELETE(pCompress)
            throw std::runtime_error("APE encoder AddData failed");
        }
        offset += n;
    }

    if (pCompress->Finish(NULL, 0, 0) != 0) {
        Logger::log(LogLevel::Error, "APE ENCODER FINISH() FAILED", "ape_encoder");
        APE_SAFE_DELETE(pCompress)
        throw std::runtime_error("APE encoder Finish failed");
    }
    APE_SAFE_DELETE(pCompress)

    if (preserve_metadata_) {
        if (!copy_apetag(input, output)) {
            Logger::log(LogLevel::Warning, "APEV2 METADATA COPY FAILED OR NOT PRESENT", "ape_encoder");
        } else {
            Logger::log(LogLevel::Debug, "APEV2 METADATA COPIED", "ape_encoder");
        }
    }

    Logger::log(LogLevel::Info, "APE RE-ENCODING COMPLETED: " + output.string(), "ape_encoder");
    return true;
}