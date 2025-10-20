//
// Created by Giuseppe Francione on 19/10/25.
//

#include "../../include/ape_processor.hpp"
#include "../../include/logger.hpp"
#include <MACLib.h>
#include <APETag.h>
#include <vector>
#include <stdexcept>
#include <filesystem>
#include <algorithm>
#include <cstring>

namespace {

bool copy_apetag(const std::filesystem::path &input,
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

} // namespace

namespace chisel {

void ApeProcessor::recompress(const std::filesystem::path& input,
                              const std::filesystem::path& output,
                              bool preserve_metadata) {
    Logger::log(LogLevel::Info, "Starting APE re-encoding: " + input.string(), "ape_processor");

    if (std::filesystem::exists(output)) {
        std::filesystem::remove(output);
    }

    int err = 0;
    APE::IAPEDecompress *pDecompress =
        CreateIAPEDecompress(input.wstring().c_str(),
                             &err,
                             true,
                             true,
                             false);
    if (!pDecompress || err != ERROR_SUCCESS) {
        if (pDecompress) delete pDecompress;
        throw std::runtime_error("ApeProcessor: cannot create APE decompress");
    }

    const unsigned sample_rate     = static_cast<unsigned>(pDecompress->GetInfo(APE::IAPEDecompress::APE_INFO_SAMPLE_RATE));
    const unsigned channels        = static_cast<unsigned>(pDecompress->GetInfo(APE::IAPEDecompress::APE_INFO_CHANNELS));
    const unsigned bits_per_sample = static_cast<unsigned>(pDecompress->GetInfo(APE::IAPEDecompress::APE_INFO_BITS_PER_SAMPLE));
    const int64_t total_frames     = pDecompress->GetInfo(APE::IAPEDecompress::APE_INFO_TOTAL_BLOCKS);

    if (channels == 0 || sample_rate == 0 || bits_per_sample == 0) {
        delete pDecompress;
        throw std::runtime_error("ApeProcessor: invalid APE file parameters");
    }

    const int bytes_per_sample = static_cast<int>(bits_per_sample / 8);
    const int block_align = static_cast<int>(channels) * bytes_per_sample;
    constexpr int block_frames_request = 16384;
    const int block_bytes = block_frames_request * block_align;

    APE::WAVEFORMATEX wfeAudioFormat{};
    FillWaveFormatEx(&wfeAudioFormat, WAVE_FORMAT_PCM,
                     static_cast<APE::int32>(sample_rate),
                     static_cast<unsigned short>(bits_per_sample),
                     static_cast<unsigned short>(channels));

    APE::IAPECompress *pCompress = CreateIAPECompress();
    if (!pCompress) {
        delete pDecompress;
        throw std::runtime_error("ApeProcessor: cannot create APE encoder");
    }

    constexpr int level = APE_COMPRESSION_LEVEL_INSANE;
    const APE::int64 maxAudioBytes = static_cast<APE::int64>(total_frames) * block_align;

    int nRetVal = pCompress->Start(
        output.wstring().c_str(),
        &wfeAudioFormat,
        false,
        maxAudioBytes,
        level,
        NULL,
        0
    );

    if (nRetVal != 0) {
        APE_SAFE_DELETE(pCompress)
        delete pDecompress;
        throw std::runtime_error("ApeProcessor: encoder start failed");
    }

    std::vector<uint8_t> block(static_cast<size_t>(block_bytes));
    int64_t frames_processed_total = 0;

    while (frames_processed_total < total_frames) {
        APE::int64 blocks_retrieved = 0;
        int rc = pDecompress->GetData(block.data(), block_frames_request, &blocks_retrieved);
        if (rc != ERROR_SUCCESS) {
            pCompress->Finish(NULL, 0, 0);
            APE_SAFE_DELETE(pCompress)
            delete pDecompress;
            throw std::runtime_error("ApeProcessor: decoding failed");
        }
        if (blocks_retrieved <= 0) break;

        const size_t bytes_to_add = static_cast<size_t>(blocks_retrieved) * static_cast<size_t>(block_align);
        int add_rc = pCompress->AddData(block.data(), static_cast<APE::int64>(bytes_to_add));
        if (add_rc != 0) {
            pCompress->Finish(NULL, 0, 0);
            APE_SAFE_DELETE(pCompress)
            delete pDecompress;
            throw std::runtime_error("ApeProcessor: AddData failed");
        }

        frames_processed_total += blocks_retrieved;
    }

    delete pDecompress;

    if (pCompress->Finish(NULL, 0, 0) != 0) {
        APE_SAFE_DELETE(pCompress)
        throw std::runtime_error("ApeProcessor: Finish failed");
    }
    APE_SAFE_DELETE(pCompress)

    if (preserve_metadata) {
        if (!copy_apetag(input, output)) {
            Logger::log(LogLevel::Warning, "APEv2 metadata copy failed or not present", "ape_processor");
        }
    }

    Logger::log(LogLevel::Info, "APE re-encoding completed: " + output.string(), "ape_processor");
}

std::string ApeProcessor::get_raw_checksum(const std::filesystem::path&) const {
    // TODO: implement checksum of raw APE data
    return "";
}

} // namespace chisel