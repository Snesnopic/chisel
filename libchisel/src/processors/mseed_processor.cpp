//
// Created by Giuseppe Francione on 19/10/25.
//

#include "mseed_processor.hpp"
#include <mseedout/mseedout.hpp>
#include <libmseed.h>
#include <stdexcept>
#include <iostream>
#include <map>
#include <cmath>
#include <memory>
#include <algorithm>
#include <mutex>
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

extern "C" void record_handler_c(char *record, const int reclen, void *userdata) {
    if (userdata == nullptr) {
        Logger::log(LogLevel::Error, "RECORD_HANDLER_C: USERDATA (FILE*) IS NULL", "MseedProcessor");
        return;
    }
    const auto outfile = static_cast<FILE*>(userdata);
    if (fwrite(record, 1, reclen, outfile) != static_cast<std::size_t>(reclen)) {
        Logger::log(LogLevel::Error, "RECORD_HANDLER_C: ERROR WRITING RECORD TO OUTPUT", "MseedProcessor");
    }
}

int MseedProcessor::choose_reclen(const uint8_t original_version,
                                  const char sampleType,
                                  const int64_t sample_count) {
    constexpr int MAX_COMPROMISE_RECLEN = 131072;
    constexpr int MIN_RECLEN = 256;

    if (original_version == 3) return MAX_COMPROMISE_RECLEN;

    std::size_t estimated_data_size = 0;
    if (sampleType == 'i' || sampleType == 'f') estimated_data_size = sample_count * 4;
    else if (sampleType == 'd') estimated_data_size = sample_count * 8;
    else estimated_data_size = sample_count;

    const std::size_t total_estimated_size = estimated_data_size + 128;
    if (total_estimated_size <= MIN_RECLEN) return MIN_RECLEN;

    const int exponent = static_cast<int>(std::ceil(std::log2(total_estimated_size)));
    const int reclen = static_cast<int>(std::pow(2, exponent));

    return std::min(reclen, MAX_COMPROMISE_RECLEN);
}

void MseedProcessor::recompress(const std::filesystem::path& input,
                                const std::filesystem::path& output, const ProcessingOptions &options) {
    Logger::log(LogLevel::Debug, "ENTERING RECOMPRESS FOR " + input.string(), get_name());

    bool success = false;
    try {
        success = mseedout::recompress_mseed(input, output);
    } catch (const std::exception& e) {
        Logger::log(LogLevel::Error, std::string("mseedout exception: ") + e.what(), get_name());
    }

    std::error_code ec;
    if (!success || std::filesystem::file_size(output, ec) == 0) {
        Logger::log(LogLevel::Debug, "NO COMPRESSIBLE DATA GENERATED OR PACKING FAILED. COPYING ORIGINAL.", get_name());
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

    const int ret_a = ms3_readtracelist(&raw_a, a.string().c_str(), nullptr, 0, MSF_UNPACKDATA, 0);
    const MstlPtr mstl_a(raw_a);
    if (ret_a != MS_NOERROR) return false;

    const int ret_b = ms3_readtracelist(&raw_b, b.string().c_str(), nullptr, 0, MSF_UNPACKDATA, 0);
    const MstlPtr mstl_b(raw_b);
    if (ret_b != MS_NOERROR) return false;

    if (!mstl_a || !mstl_b) return mstl_a.get() == mstl_b.get();
    if (mstl_a->numtraceids != mstl_b->numtraceids) return false;

    const MS3TraceID *id_a = mstl_a->traces.next[0];
    const MS3TraceID *id_b = mstl_b->traces.next[0];

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