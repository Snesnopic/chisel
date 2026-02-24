//
// Created by Giuseppe Francione on 19/10/25.
//

#include "mseed_processor.hpp"
#include <libmseed.h>
#include <stdexcept>
#include <iostream>
#include <map>
#include "file_utils.hpp"
#include "logger.hpp"
#include "log_sink.hpp"

namespace chisel {

extern "C" void record_handler_c(char *record, int reclen, void *userdata) {
    if (userdata == nullptr) {
        Logger::log(LogLevel::Error, "Record_handler_c: userdata (file*) is null", "MseedProcessor");
        return;
    }

    auto outfile = static_cast<FILE*>(userdata);

    if (fwrite(record, 1, reclen, outfile) != static_cast<size_t>(reclen)) {
        Logger::log(LogLevel::Error, "Record_handler_c: error writing record to output file", "MseedProcessor");
    }
}
int MseedProcessor::choose_reclen(const uint8_t original_version,
                                  const char sampleType,
                                  const int64_t sample_count) {

    constexpr int MAX_COMPROMISE_RECLEN = 131072;

    constexpr int MIN_RECLEN = 256;

    if (original_version == 3) {

        return MAX_COMPROMISE_RECLEN;
    }


    size_t estimated_data_size = 0;

    if (sampleType == 'i' || sampleType == 'f') {
        estimated_data_size = sample_count * 4; // float32 or int32
    } else if (sampleType == 'd') {
        estimated_data_size = sample_count * 8; // float64
    } else {
        estimated_data_size = sample_count; // text
    }

    const size_t total_estimated_size = estimated_data_size + 128;

    if (total_estimated_size <= MIN_RECLEN) {
        return MIN_RECLEN;
    }

    const int exponent = static_cast<int>(std::ceil(std::log2(total_estimated_size)));
    int reclen = static_cast<int>(std::pow(2, exponent));
    if (reclen <= MIN_RECLEN) {
        return reclen;
    }
    return MIN_RECLEN;
}
void MseedProcessor::recompress(const std::filesystem::path& input,
                                const std::filesystem::path& output, const ProcessingOptions &options) {
    Logger::log(LogLevel::Debug, "Entering recompress for " + input.string(), get_name());

    ms_rloginit(nullptr, nullptr, [](const char* _){}, nullptr, 0);
    MS3Record *msr = nullptr;
    MS3TraceList *mstl = nullptr;
    FILE *outfile = nullptr;
    uint8_t original_version = 3;
    uint32_t pack_flags = MSF_FLUSHDATA;

    int ret = ms3_readmsr(&msr, input.string().c_str(), 0, 0);

    if (ret != MS_NOERROR) {
        if (msr) msr3_free(&msr);
        Logger::log(LogLevel::Warning, "Could not peek first record from " + input.string() +
                                       " (ret " + std::to_string(ret) + "). will attempt full read.", get_name());
    }

    if (msr) {
        original_version = msr->formatversion;
        msr3_free(&msr);
    }

    if (original_version == 2) {
        pack_flags |= MSF_PACKVER2;
    }

    ret = ms3_readtracelist(&mstl, input.string().c_str(), nullptr, 0, MSF_UNPACKDATA, 0);

    if (ret != MS_NOERROR) {
        if (mstl) mstl3_free(&mstl, 0);
        throw std::runtime_error("Failed to read trace list from input: " + input.string());
    }

    if (mstl == nullptr) {
        return;
    }

    outfile = chisel::open_file(output.string().c_str(), "wb");
    if (outfile == nullptr) {
        mstl3_free(&mstl, 0);
        throw std::runtime_error("Failed to open output file for writing: " + output.string());
    }

    for (MS3TraceID *id = mstl->traces.next[0]; id != nullptr; id = id->next[0]) {
        for (MS3TraceSeg *seg = id->first; seg != nullptr; seg = seg->next) {

            int8_t target_encoding;

            const int reclen = choose_reclen(original_version, seg->sampletype, seg->samplecnt);
            int64_t packed_samples = 0;
            int64_t ret_pack = 0;

            if (seg->sampletype == 'i') {
                target_encoding = DE_STEIM2;
            }
            else if (seg->sampletype == 'f') {
                target_encoding = DE_FLOAT32;
            } else if (seg->sampletype == 'd') {
                target_encoding = DE_FLOAT64;
            } else if (seg->sampletype == 't') {
                target_encoding = DE_TEXT;
            } else {
                target_encoding = -1;
            }

            ret_pack = mstl3_pack_segment(mstl, id, seg,
                                          record_handler_c, outfile,
                                          reclen, target_encoding,
                                          &packed_samples, pack_flags, 0, nullptr);

            if (ret_pack < 0 && seg->sampletype == 'i') {

                Logger::log(LogLevel::Warning, std::string("Sid ") + id->sid +
                                               ": steim2 packing failed (data range likely too large). " +
                                               "Retrying with uncompressed 32-bit integers (de_int32).", get_name());

                target_encoding = DE_INT32;
                packed_samples = 0;

                ret_pack = mstl3_pack_segment(mstl, id, seg,
                                              record_handler_c, outfile,
                                              reclen, target_encoding,
                                              &packed_samples, pack_flags, 0, nullptr);
            }

            if (ret_pack < 0) {
                Logger::log(LogLevel::Error, std::string("Final packing error for sid ") + id->sid +
                                             " (type " + seg->sampletype + ") after fallback. Segment skipped.", get_name());
            }
        }
    }

    fclose(outfile);
    mstl3_free(&mstl, 0);
    Logger::log(LogLevel::Debug, "Exiting recompress for " + output.string(), get_name());
}

std::string MseedProcessor::get_raw_checksum(const std::filesystem::path&) const {
    // TODO: implement checksum of raw MiniSEED data
    return "";
}

bool MseedProcessor::raw_equal(const std::filesystem::path &a,
                               const std::filesystem::path &b) const {

    MS3TraceList *mstl_a = nullptr;
    MS3TraceList *mstl_b = nullptr;
    bool are_equal = true;

    int ret_a = ms3_readtracelist(&mstl_a, a.string().c_str(), nullptr, 0, MSF_UNPACKDATA, 0);
    if (ret_a != MS_NOERROR) {
        Logger::log(LogLevel::Error, "Raw_equal: Failed to read file a: " + a.string(), get_name());
        if (mstl_a) mstl3_free(&mstl_a, 0);
        return false;
    }

    int ret_b = ms3_readtracelist(&mstl_b, b.string().c_str(), nullptr, 0, MSF_UNPACKDATA, 0);
    if (ret_b != MS_NOERROR) {
        Logger::log(LogLevel::Error, "Raw_equal: Failed to read file b: " + b.string(), get_name());
        if (mstl_a) mstl3_free(&mstl_a, 0);
        if (mstl_b) mstl3_free(&mstl_b, 0);
        return false;
    }

    if (mstl_a == nullptr || mstl_b == nullptr) {
        are_equal = (mstl_a == mstl_b);
        if (mstl_a) mstl3_free(&mstl_a, 0);
        if (mstl_b) mstl3_free(&mstl_b, 0);

        return are_equal;
    }

    if (mstl_a->numtraceids != mstl_b->numtraceids) {
        Logger::log(LogLevel::Warning, "Raw_equal: trace id count mismatch (a: " +
                                       std::to_string(mstl_a->numtraceids) + ", b: " +
                                       std::to_string(mstl_b->numtraceids) + ")", get_name());
        are_equal = false;
        if (mstl_a) mstl3_free(&mstl_a, 0);
        if (mstl_b) mstl3_free(&mstl_b, 0);

        return are_equal;
    }

    MS3TraceID *id_a = mstl_a->traces.next[0];
    MS3TraceID *id_b = mstl_b->traces.next[0];

    while (id_a != nullptr && id_b != nullptr) {
        if (strcmp(id_a->sid, id_b->sid) != 0) {
            Logger::log(LogLevel::Warning, std::string("Raw_equal: sid mismatch (a: ") + id_a->sid +
                                           ", b: " + id_b->sid + ")", get_name());
            are_equal = false;
            if (mstl_a) mstl3_free(&mstl_a, 0);
            if (mstl_b) mstl3_free(&mstl_b, 0);

            return are_equal;
        }

        if (id_a->numsegments != id_b->numsegments) {
            Logger::log(LogLevel::Warning, std::string("Raw_equal: segment count mismatch for ") + id_a->sid +
                                           " (a: " + std::to_string(id_a->numsegments) +
                                           ", b: " + std::to_string(id_b->numsegments) + ")", get_name());
            are_equal = false;
            if (mstl_a) mstl3_free(&mstl_a, 0);
            if (mstl_b) mstl3_free(&mstl_b, 0);

            return are_equal;
        }

        const MS3TraceSeg *seg_a = id_a->first;
        const MS3TraceSeg *seg_b = id_b->first;

        while (seg_a != nullptr && seg_b != nullptr) {
            if (seg_a->starttime != seg_b->starttime ||
                seg_a->samplecnt != seg_b->samplecnt ||
                seg_a->sampletype != seg_b->sampletype)
            {
                Logger::log(LogLevel::Warning, std::string("Raw_equal: segment metadata mismatch for ") + id_a->sid, get_name());
                are_equal = false;
                if (mstl_a) mstl3_free(&mstl_a, 0);
                if (mstl_b) mstl3_free(&mstl_b, 0);

                return are_equal;
            }

            if (!MS_ISRATETOLERABLE(seg_a->samprate, seg_b->samprate)) {
                Logger::log(LogLevel::Warning, "Raw_equal: sample rate mismatch for " + std::string(id_a->sid) +
                                               " (a: " + std::to_string(seg_a->samprate) +
                                               ", b: " + std::to_string(seg_b->samprate) + ")", get_name());
are_equal = false;
                if (mstl_a) mstl3_free(&mstl_a, 0);
                if (mstl_b) mstl3_free(&mstl_b, 0);
                return are_equal;
            }

            if (seg_a->sampletype == 'i') { // 32-bit integers
                const auto *samples_a = static_cast<int32_t*>(seg_a->datasamples);
                const auto *samples_b = static_cast<int32_t*>(seg_b->datasamples);
                for (int64_t i = 0; i < seg_a->samplecnt; ++i) {
                    if (samples_a[i] != samples_b[i]) {
                        are_equal = false;
                        if (mstl_a) mstl3_free(&mstl_a, 0);
                        if (mstl_b) mstl3_free(&mstl_b, 0);
                        return are_equal;
                    }
                }
            } else if (seg_a->sampletype == 'f') { // 32-bit floats
                const auto *samples_a = static_cast<float*>(seg_a->datasamples);
                const auto *samples_b = static_cast<float*>(seg_b->datasamples);
                for (int64_t i = 0; i < seg_a->samplecnt; ++i) {
                    if (std::fabs(samples_a[i] - samples_b[i]) > (std::fabs(samples_a[i]) * 1e-6)) {
                        are_equal = false;
                        if (mstl_a) mstl3_free(&mstl_a, 0);
                        if (mstl_b) mstl3_free(&mstl_b, 0);
                        return are_equal;
                    }
                }
            } else if (seg_a->sampletype == 'd') { // 64-bit doubles
                const auto *samples_a = static_cast<double*>(seg_a->datasamples);
                const auto *samples_b = static_cast<double*>(seg_b->datasamples);
                for (int64_t i = 0; i < seg_a->samplecnt; ++i) {
                    if (std::fabs(samples_a[i] - samples_b[i]) > (std::fabs(samples_a[i]) * 1e-9)) {
                        are_equal = false;
                        if (mstl_a) mstl3_free(&mstl_a, 0);
                        if (mstl_b) mstl3_free(&mstl_b, 0);
                        return are_equal;
                    }
                }
            } else if (seg_a->sampletype == 't') { // Text
                if (memcmp(seg_a->datasamples, seg_b->datasamples, seg_a->samplecnt) != 0) {
                    are_equal = false;
                    if (mstl_a) mstl3_free(&mstl_a, 0);
                    if (mstl_b) mstl3_free(&mstl_b, 0);
                    return are_equal;
                }
            } else {
                Logger::log(LogLevel::Error, std::string("Raw_equal: unknown sample type '") + seg_a->sampletype + "'", get_name());
                are_equal = false;
                if (mstl_a) mstl3_free(&mstl_a, 0);
                if (mstl_b) mstl3_free(&mstl_b, 0);
                return are_equal;
            }

            seg_a = seg_a->next;
            seg_b = seg_b->next;
        }

        id_a = id_a->next[0];
        id_b = id_b->next[0];
    }

    if (mstl_a) mstl3_free(&mstl_a, 0);
    if (mstl_b) mstl3_free(&mstl_b, 0);
    return are_equal;
}

} // namespace chisel