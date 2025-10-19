//
// Created by Giuseppe Francione on 19/10/25.
//

#include "mseed_processor.hpp"
#include <libmseed.h>
#include <limits>
#include <vector>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace chisel {

void MseedProcessor::recompress(const std::filesystem::path& input,
                                const std::filesystem::path& output,
                                bool /*preserve_metadata*/) {
    Logger::log(LogLevel::Info, "Starting MiniSEED recompression", "mseed_processor");

    MS3Record* msr = nullptr;
    uint32_t read_flags = MSF_VALIDATECRC | MSF_UNPACKDATA;
    constexpr int8_t verbose = 0;

    int retcode = ms3_readmsr(&msr, input.string().c_str(), read_flags, verbose);
    while (retcode == MS_NOERROR) {
        const int reclen = choose_reclen(msr, msr->numsamples);

        uint32_t write_flags = MSF_FLUSHDATA;
        if (msr->formatversion == 2) write_flags |= MSF_PACKVER2;
        if (msr->sampletype == 'i') msr->encoding = DE_STEIM2;

        msr->reclen = reclen;

        const int64_t rv = msr3_writemseed(msr, output.string().c_str(), 1, write_flags, verbose);
        if (rv < 0) {
            Logger::log(LogLevel::Error, "Error writing MiniSEED record", "mseed_processor");
            ms3_readmsr(&msr, nullptr, read_flags, 0);
            throw std::runtime_error("MseedProcessor: write failed");
        }

        retcode = ms3_readmsr(&msr, nullptr, read_flags, verbose);
    }

    if (retcode != MS_ENDOFFILE) {
        Logger::log(LogLevel::Error, "Error reading MiniSEED: " + std::string(ms_errorstr(retcode)), "mseed_processor");
        ms3_readmsr(&msr, nullptr, read_flags, 0);
        throw std::runtime_error("MseedProcessor: read failed");
    }

    ms3_readmsr(&msr, nullptr, read_flags, 0);
    Logger::log(LogLevel::Info, "Recompression completed successfully", "mseed_processor");
}

int MseedProcessor::choose_reclen(const MS3Record* msr, size_t sample_count) {
    if (!msr || sample_count == 0) return 4096;

    const int max_reclen_exp = (msr->formatversion == 2) ? 16 : 20;
    constexpr int MIN_RECLEN_EXP = 8;
    std::vector<int> candidates;
    for (int e = MIN_RECLEN_EXP; e <= max_reclen_exp; ++e)
        candidates.push_back(1 << e);

    const auto tmpdir = std::filesystem::temp_directory_path();
    uint64_t best_size = (std::numeric_limits<uint64_t>::max)();
    int best_reclen = 4096;

    for (int reclen : candidates) {
        MS3Record* clone = msr3_duplicate(msr, 1);
        if (!clone) continue;

        clone->reclen = reclen;
        if (clone->sampletype == 'i') clone->encoding = DE_STEIM2;

        auto tmpfile = tmpdir / ("mseed_test_" + std::to_string(reclen) + ".mseed");

        uint32_t write_flags = MSF_FLUSHDATA;
        if (clone->formatversion == 2) write_flags |= MSF_PACKVER2;

        const int64_t rv = msr3_writemseed(clone, tmpfile.string().c_str(), 1, write_flags, 0);
        msr3_free(&clone);

        if (rv < 0) {
            if (std::filesystem::exists(tmpfile)) std::filesystem::remove(tmpfile);
            continue;
        }

        const uint64_t sz = std::filesystem::file_size(tmpfile);
        if (sz < best_size) {
            best_size = sz;
            best_reclen = reclen;
        }
        std::filesystem::remove(tmpfile);
    }

    return best_reclen;
}

std::string MseedProcessor::get_raw_checksum(const std::filesystem::path&) const {
    // TODO: implement checksum of raw MiniSEED data
    return "";
}

} // namespace chisel