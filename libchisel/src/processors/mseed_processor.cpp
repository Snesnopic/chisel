//
// Created by Giuseppe Francione on 19/10/25.
//

#include "mseed_processor.hpp"
#include <libmseed.h>
#include <stdexcept>
#include <iostream>
#include <map>
#include <cmath>
#include <memory>
#include <algorithm>
#include "file_utils.hpp"
#include "logger.hpp"
#include "log_sink.hpp"

#ifdef _WIN32
#undef min
#undef max
#endif

namespace chisel {

struct MstlDeleter {
    void operator()(MS3TraceList* mstl) const {
        if (mstl) mstl3_free(&mstl, 0);
    }
};
using MstlPtr = std::unique_ptr<MS3TraceList, MstlDeleter>;

struct FileDeleter {
    void operator()(FILE* fp) const {
        if (fp) fclose(fp);
    }
};
using FilePtr = std::unique_ptr<FILE, FileDeleter>;

extern "C" void record_handler_c(char *record, int reclen, void *userdata) {
    if (userdata == nullptr) {
        Logger::log(LogLevel::Error, "RECORD_HANDLER_C: USERDATA (FILE*) IS NULL", "MseedProcessor");
        return;
    }
    auto outfile = static_cast<FILE*>(userdata);
    if (fwrite(record, 1, reclen, outfile) != static_cast<size_t>(reclen)) {
        Logger::log(LogLevel::Error, "RECORD_HANDLER_C: ERROR WRITING RECORD TO OUTPUT", "MseedProcessor");
    }
}

int MseedProcessor::choose_reclen(const uint8_t original_version,
                                  const char sampleType,
                                  const int64_t sample_count) {
    constexpr int MAX_COMPROMISE_RECLEN = 131072;
    constexpr int MIN_RECLEN = 256;

    if (original_version == 3) return MAX_COMPROMISE_RECLEN;

    size_t estimated_data_size = 0;
    if (sampleType == 'i' || sampleType == 'f') estimated_data_size = sample_count * 4;
    else if (sampleType == 'd') estimated_data_size = sample_count * 8;
    else estimated_data_size = sample_count;

    const size_t total_estimated_size = estimated_data_size + 128;
    if (total_estimated_size <= MIN_RECLEN) return MIN_RECLEN;

    const int exponent = static_cast<int>(std::ceil(std::log2(total_estimated_size)));
    int reclen = static_cast<int>(std::pow(2, exponent));

    return std::min(reclen, MAX_COMPROMISE_RECLEN);
}

void MseedProcessor::recompress(const std::filesystem::path& input,
                                const std::filesystem::path& output, const ProcessingOptions &options) {
    Logger::log(LogLevel::Debug, "ENTERING RECOMPRESS FOR " + input.string(), get_name());

    ms_rloginit(nullptr, nullptr, [](const char*){}, nullptr, 0);
    MS3Record *msr = nullptr;
    uint8_t original_version = 3;
    uint32_t pack_flags = MSF_FLUSHDATA;

    int ret = ms3_readmsr(&msr, input.string().c_str(), 0, 0);
    if (ret != MS_NOERROR) {
        Logger::log(LogLevel::Warning, "COULD NOT PEEK FIRST RECORD, ATTEMPTING FULL READ.", get_name());
    } else {
        original_version = msr->formatversion;
    }
    if (msr != nullptr) {
        // force libmseed to close the file handle
        ms3_readmsr(&msr, nullptr, 0, 0);
        msr3_free(&msr);
    }

    if (original_version == 2) pack_flags |= MSF_PACKVER2;

    MS3TraceList *raw_mstl = nullptr;
    ret = ms3_readtracelist(&raw_mstl, input.string().c_str(), nullptr, 0, MSF_UNPACKDATA, 0);
    MstlPtr mstl(raw_mstl);

    if (ret != MS_NOERROR) {
        throw std::runtime_error("failed to read trace list. libmseed code: " + std::to_string(ret));
    }

    bool wrote_any_data = false;

    // scope limits fileptr lifetime to ensure flush/close before size check
    {
        FilePtr outfile(chisel::open_file(output.string().c_str(), "wb"));
        if (!outfile) {
            throw std::runtime_error("failed to open output file for writing");
        }

        if (mstl && mstl->traces.next[0]) {
            for (MS3TraceID *id = mstl->traces.next[0]; id != nullptr; id = id->next[0]) {
                for (MS3TraceSeg *seg = id->first; seg != nullptr; seg = seg->next) {
                    if (seg->samplecnt <= 0) continue;

                    int8_t target_encoding = -1;
                    const int reclen = choose_reclen(original_version, seg->sampletype, seg->samplecnt);
                    int64_t packed_samples = 0;

                    if (seg->sampletype == 'i') target_encoding = DE_STEIM2;
                    else if (seg->sampletype == 'f') target_encoding = DE_FLOAT32;
                    else if (seg->sampletype == 'd') target_encoding = DE_FLOAT64;
                    else if (seg->sampletype == 't') target_encoding = DE_TEXT;

                    int64_t ret_pack = mstl3_pack_segment(mstl.get(), id, seg,
                                                          record_handler_c, outfile.get(),
                                                          reclen, target_encoding,
                                                          &packed_samples, pack_flags, 0, nullptr);

                    if (ret_pack < 0 && seg->sampletype == 'i') {
                        Logger::log(LogLevel::Warning, std::string("SID ") + id->sid +
                                                       ": STEIM2 PACKING FAILED. RETRYING DE_INT32.", get_name());
                        target_encoding = DE_INT32;
                        packed_samples = 0;
                        ret_pack = mstl3_pack_segment(mstl.get(), id, seg,
                                                      record_handler_c, outfile.get(),
                                                      reclen, target_encoding,
                                                      &packed_samples, pack_flags, 0, nullptr);
                    }

                    if (ret_pack > 0 || packed_samples > 0) {
                        wrote_any_data = true;
                    } else if (ret_pack < 0) {
                        Logger::log(LogLevel::Error, std::string("FINAL PACKING ERROR FOR SID ") + id->sid, get_name());
                    }
                }
            }
        }
    }

    // fallback: if packing did not process actual samples or file size is 0
    std::error_code ec;
    if (!wrote_any_data || std::filesystem::file_size(output, ec) == 0) {
        Logger::log(LogLevel::Debug, "NO COMPRESSIBLE DATA GENERATED. COPYING ORIGINAL.", get_name());
        std::filesystem::copy_file(input, output, std::filesystem::copy_options::overwrite_existing);
    }

    Logger::log(LogLevel::Debug, "EXITING RECOMPRESS FOR " + output.string(), get_name());
}

std::string MseedProcessor::get_raw_checksum(const std::filesystem::path&) const {
    return "";
}

bool MseedProcessor::raw_equal(const std::filesystem::path &a,
                               const std::filesystem::path &b) const {

    MS3TraceList *raw_a = nullptr, *raw_b = nullptr;

    int ret_a = ms3_readtracelist(&raw_a, a.string().c_str(), nullptr, 0, MSF_UNPACKDATA, 0);
    MstlPtr mstl_a(raw_a);
    if (ret_a != MS_NOERROR) return false;

    int ret_b = ms3_readtracelist(&raw_b, b.string().c_str(), nullptr, 0, MSF_UNPACKDATA, 0);
    MstlPtr mstl_b(raw_b);
    if (ret_b != MS_NOERROR) return false;

    if (!mstl_a || !mstl_b) return mstl_a.get() == mstl_b.get();
    if (mstl_a->numtraceids != mstl_b->numtraceids) return false;

    MS3TraceID *id_a = mstl_a->traces.next[0];
    MS3TraceID *id_b = mstl_b->traces.next[0];

    while (id_a != nullptr && id_b != nullptr) {
        if (strcmp(id_a->sid, id_b->sid) != 0) return false;
        if (id_a->numsegments != id_b->numsegments) return false;

        const MS3TraceSeg *seg_a = id_a->first;
        const MS3TraceSeg *seg_b = id_b->first;

        while (seg_a != nullptr && seg_b != nullptr) {
            if (seg_a->starttime != seg_b->starttime ||
                seg_a->samplecnt != seg_b->samplecnt ||
                seg_a->sampletype != seg_b->sampletype) return false;

            if (!MS_ISRATETOLERABLE(seg_a->samprate, seg_b->samprate)) return false;

            if (seg_a->sampletype == 'i') {
                const auto *samples_a = static_cast<int32_t*>(seg_a->datasamples);
                const auto *samples_b = static_cast<int32_t*>(seg_b->datasamples);
                for (int64_t i = 0; i < seg_a->samplecnt; ++i) {
                    if (samples_a[i] != samples_b[i]) return false;
                }
            } else if (seg_a->sampletype == 'f') {
                const auto *samples_a = static_cast<float*>(seg_a->datasamples);
                const auto *samples_b = static_cast<float*>(seg_b->datasamples);
                for (int64_t i = 0; i < seg_a->samplecnt; ++i) {
                    if (std::fabs(samples_a[i] - samples_b[i]) > std::max(1e-6f, std::fabs(samples_a[i]) * 1e-6f)) return false;
                }
            } else if (seg_a->sampletype == 'd') {
                const auto *samples_a = static_cast<double*>(seg_a->datasamples);
                const auto *samples_b = static_cast<double*>(seg_b->datasamples);
                for (int64_t i = 0; i < seg_a->samplecnt; ++i) {
                    if (std::fabs(samples_a[i] - samples_b[i]) > std::max(1e-9, std::fabs(samples_a[i]) * 1e-9)) return false;
                }
            } else if (seg_a->sampletype == 't') {
                if (memcmp(seg_a->datasamples, seg_b->datasamples, seg_a->samplecnt) != 0) return false;
            } else {
                return false;
            }

            seg_a = seg_a->next;
            seg_b = seg_b->next;
        }
        id_a = id_a->next[0];
        id_b = id_b->next[0];
    }
    return true;
}

} // namespace chisel