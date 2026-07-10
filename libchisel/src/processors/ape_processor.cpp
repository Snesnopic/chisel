//
// Created by Giuseppe Francione on 19/10/25.
//

#include "../../include/ape_processor.hpp"
#include "../../include/logger.hpp"
#include "../../include/audio_metadata_util.hpp"
#include <MACLib.h>
#include "CharacterHelper.h"
#include <APETag.h>
#include <vector>
#include <stdexcept>
#include <filesystem>
#include <algorithm>
#include <cstring>
#include <any>


namespace {

/**
 * @brief Copies an APEv2 tag from an input file to an output file.
 * @param input The path to the source file with the APE tag.
 * @param output The path to the destination file.
 * @return True if the tag was copied successfully, false otherwise.
 */
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
            if (field == nullptr) break;

            const APE::str_utfn *name = field->GetFieldName();
            const char *value = field->GetFieldValue();
            const int valueSize = field->GetFieldValueSize();
            const int flags = field->GetFieldFlags();
            if ((name == nullptr) || valueSize <= 0) continue;

            const bool isBinary = (flags & TAG_FIELD_FLAG_DATA_TYPE_BINARY) != 0;

            if (isBinary) {
                outTag.SetFieldBinary(name, value, valueSize, flags);
            } else {
                outTag.SetFieldString(name, value, true, nullptr);
            }
        }

        return outTag.Save() != 0;
    } catch (const std::exception& e) {
        // log known exceptions
        chisel::Logger::log(chisel::LogLevel::Warning, "Failed to copy APE tag: " + std::string(e.what()), "ApeProcessor");
        return false;
    } catch (...) {
        // log unknown exceptions
        chisel::Logger::log(chisel::LogLevel::Warning, "Failed to copy APE tag: Unknown exception caught", "ApeProcessor");
        return false;
    }
}

/**
 * @brief RAII guard that provides a path to open with MACLib, guaranteeing a
 * recognized extension (.ape/.mac/.apl) as CreateIAPEDecompress requires one and
 * fails outright otherwise -- notably breaking on the executor's own pipeline temp
 * files, which are always suffixed ".tmp" regardless of the original format.
 */
class ApeExtensionGuard {
public:
    explicit ApeExtensionGuard(const std::filesystem::path& original) : path_(original) {
        std::string ext = original.extension().string();
        for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext == ".ape" || ext == ".mac" || ext == ".apl") return;

        temp_link_ = original.parent_path() / (original.filename().string() + ".ape");
        std::error_code ec;
        std::filesystem::create_hard_link(original, temp_link_, ec);
        if (ec) {
            // cross-filesystem or unsupported hardlink; fall back to a copy
            std::filesystem::copy_file(original, temp_link_, std::filesystem::copy_options::overwrite_existing, ec);
        }
        if (ec) {
            temp_link_.clear();
            throw std::runtime_error("ApeProcessor: failed to prepare a .ape-suffixed path for decoding");
        }
        path_ = temp_link_;
    }

    ~ApeExtensionGuard() {
        if (!temp_link_.empty()) {
            std::error_code ec;
            std::filesystem::remove(temp_link_, ec);
        }
    }

    ApeExtensionGuard(const ApeExtensionGuard&) = delete;
    ApeExtensionGuard& operator=(const ApeExtensionGuard&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
    std::filesystem::path temp_link_;
};

} // namespace

namespace chisel {

void ApeProcessor::recompress(const std::filesystem::path& input,
                              const std::filesystem::path& output, const ProcessingOptions &options) {
    Logger::log(LogLevel::Debug, "Entering recompress for " + input.string(), get_name());

    if (std::filesystem::exists(output)) {
        std::filesystem::remove(output);
    }

    int err = 0;

    const auto *const pMacString = APE::CAPECharacterHelper::GetUTFNFromANSI(input.string().c_str());

    APE::IAPEDecompress *pDecompress = CreateIAPEDecompress(
        pMacString, &err, true, true, false);

    if (pDecompress == nullptr || err != ERROR_SUCCESS) {
        delete pDecompress;
        delete[] pMacString;
        throw std::runtime_error("ApeProcessor: cannot create APE decompress (err: " + std::to_string(err) + ")");
    }

    const unsigned sample_rate     = static_cast<unsigned>(pDecompress->GetInfo(APE::IAPEDecompress::APE_INFO_SAMPLE_RATE));
    const unsigned channels        = static_cast<unsigned>(pDecompress->GetInfo(APE::IAPEDecompress::APE_INFO_CHANNELS));
    const unsigned bits_per_sample = static_cast<unsigned>(pDecompress->GetInfo(APE::IAPEDecompress::APE_INFO_BITS_PER_SAMPLE));
    const int64_t total_frames     = pDecompress->GetInfo(APE::IAPEDecompress::APE_INFO_TOTAL_BLOCKS);

    if (channels == 0 || sample_rate == 0 || bits_per_sample == 0) {
        delete pDecompress;
        delete[] pMacString;
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
    if (pCompress == nullptr) {
        delete pDecompress;
        delete[] pMacString;
        throw std::runtime_error("ApeProcessor: cannot create APE encoder");
    }

    constexpr int level = APE_COMPRESSION_LEVEL_INSANE;
    const APE::int64 maxAudioBytes = static_cast<APE::int64>(total_frames) * block_align;

    const int nRetVal = pCompress->Start(
        output.wstring().c_str(),
        &wfeAudioFormat,
        false,
        maxAudioBytes,
        level,
        nullptr,
        0
    );

    if (nRetVal != 0) {
        APE_SAFE_DELETE(pCompress)
        delete pDecompress;
        delete[] pMacString;
        throw std::runtime_error("ApeProcessor: encoder start failed");
    }

    std::vector<uint8_t> block(static_cast<std::size_t>(block_bytes));
    int64_t frames_processed_total = 0;

    while (frames_processed_total < total_frames) {
        APE::int64 blocks_retrieved = 0;
        const int rc = pDecompress->GetData(block.data(), block_frames_request, &blocks_retrieved);
        if (rc != ERROR_SUCCESS) {
            pCompress->Finish(nullptr, 0, 0);
            APE_SAFE_DELETE(pCompress)
            delete pDecompress;
            delete[] pMacString;
            throw std::runtime_error("ApeProcessor: decoding failed");
        }
        if (blocks_retrieved <= 0) break;

        const std::size_t bytes_to_add = static_cast<std::size_t>(blocks_retrieved) * static_cast<std::size_t>(block_align);
        const int add_rc = pCompress->AddData(block.data(), static_cast<APE::int64>(bytes_to_add));
        if (add_rc != 0) {
            pCompress->Finish(nullptr, 0, 0);
            APE_SAFE_DELETE(pCompress)
            delete pDecompress;
            delete[] pMacString;
            throw std::runtime_error("ApeProcessor: AddData failed");
        }

        frames_processed_total += blocks_retrieved;
    }

    const int fin_rc = pCompress->Finish(nullptr, 0, 0);
    if (fin_rc != 0) {
        APE_SAFE_DELETE(pCompress)
        delete pDecompress;
        delete[] pMacString;
        throw std::runtime_error("ApeProcessor: Finish failed");
    }
    APE_SAFE_DELETE(pCompress)
    delete pDecompress;
    delete[] pMacString;

    if (options.preserve_metadata) {
        if (!copy_apetag(input, output)) {
            Logger::log(LogLevel::Debug, "APEv2 metadata copy skipped or failed", get_name());
        }
    }

    Logger::log(LogLevel::Debug, "Exiting recompress for " + output.string(), get_name());
}

std::optional<ExtractedContent> ApeProcessor::prepare_extraction(const std::filesystem::path& input_path) {
    return AudioMetadataUtil::prepareCoverExtraction(input_path, "ape-processor", get_name());
}

std::filesystem::path ApeProcessor::finalize_extraction(const ExtractedContent &content, const ProcessingOptions &options) {
    return AudioMetadataUtil::finalizeCoverExtraction(content, get_name());
}

std::string ApeProcessor::get_raw_checksum(const std::filesystem::path&) const {
    // TODO: implement checksum of raw APE data
    return "";
}

/**
 * @brief Decodes an APE file into a raw PCM audio buffer.
 * @param file The path to the APE file.
 * @param sample_rate Output parameter for the sample rate.
 * @param channels Output parameter for the number of channels.
 * @param bps Output parameter for the bits per sample.
 * @return A vector of 32-bit integers representing the decoded PCM data.
 */
std::vector<int32_t> decode_ape_pcm(const std::filesystem::path& file,
                                    unsigned& sample_rate,
                                    unsigned& channels,
                                    unsigned& bps) {
    int err = 0;

    const ApeExtensionGuard ext_guard(file);
    const auto *const pMacString = APE::CAPECharacterHelper::GetUTFNFromANSI(ext_guard.path().string().c_str());

    APE::IAPEDecompress* dec = CreateIAPEDecompress(pMacString,
                                                    &err,
                                                    true,  // full header analysis
                                                    true,  // check CRC
                                                    false);
    if ((dec == nullptr) || err != ERROR_SUCCESS) {
        delete dec;
        throw std::runtime_error("APE open failed (err: " + std::to_string(err) + ")");
    }

    sample_rate = static_cast<unsigned>(dec->GetInfo(APE::IAPEDecompress::APE_INFO_SAMPLE_RATE));
    channels    = static_cast<unsigned>(dec->GetInfo(APE::IAPEDecompress::APE_INFO_CHANNELS));
    bps         = static_cast<unsigned>(dec->GetInfo(APE::IAPEDecompress::APE_INFO_BITS_PER_SAMPLE));
    const int64_t total_blocks = dec->GetInfo(APE::IAPEDecompress::APE_INFO_TOTAL_BLOCKS);

    if (channels == 0 || sample_rate == 0 || bps == 0) {
        delete dec;
        throw std::runtime_error("Invalid APE parameters");
    }

    const int bytes_per_sample =static_cast<int>(bps) / 8;
    const int block_align = static_cast<int>(channels) * bytes_per_sample;
    constexpr int block_frames_request = 16384;

    std::vector<int32_t> pcm;
    std::vector<uint8_t> block(static_cast<std::size_t>(block_frames_request) * block_align);

    int64_t frames_processed = 0;
    while (frames_processed < total_blocks) {
        APE::int64 blocks_retrieved = 0;
        const int rc = dec->GetData(block.data(), block_frames_request, &blocks_retrieved);
        if (rc != ERROR_SUCCESS) {
            delete dec;
            throw std::runtime_error("APE decode failed");
        }
        if (blocks_retrieved <= 0) break;

        const std::size_t bytes_to_copy = static_cast<std::size_t>(blocks_retrieved) * block_align;
        const auto* src16 = reinterpret_cast<const int16_t*>(block.data());
        const auto* src32 = reinterpret_cast<const int32_t*>(block.data());

        if (bps == 16) {
            for (size_t i = 0; i < bytes_to_copy / 2; ++i) {
                pcm.push_back(static_cast<int32_t>(src16[i]));
            }
        } else if (bps == 24) {
            // packed 3 bytes per sample (like WAV), NOT 4-byte aligned like int32_t
            for (size_t off = 0; off + 3 <= bytes_to_copy; off += 3) {
                int32_t sample = static_cast<int32_t>(block[off]) |
                                 (static_cast<int32_t>(block[off + 1]) << 8) |
                                 (static_cast<int32_t>(block[off + 2]) << 16);
                if (sample & 0x00800000) sample |= static_cast<int32_t>(0xFF000000);
                pcm.push_back(sample);
            }
        } else if (bps == 32) {
            for (size_t i = 0; i < bytes_to_copy / 4; ++i) {
                pcm.push_back(src32[i]);
            }
        } else {
            delete dec;
            throw std::runtime_error("Unsupported bit depth in APE");
        }

        frames_processed += blocks_retrieved;
    }

    delete dec;
    return pcm;
}

bool ApeProcessor::raw_equal(const std::filesystem::path& a,
                             const std::filesystem::path& b) const {
    try {
        unsigned ra, ca, bpsa;
        unsigned rb, cb, bpsb;
        const auto pcmA = decode_ape_pcm(a, ra, ca, bpsa);
        const auto pcmB = decode_ape_pcm(b, rb, cb, bpsb);

        if (ra != rb || ca != cb || bpsa != bpsb) return false;
        return pcmA == pcmB;
    } catch (const std::exception& e) {
        Logger::log(LogLevel::Error, std::string("raw_equal failed: ") + e.what(), get_name());
        return false;
    }
}

} // namespace chisel