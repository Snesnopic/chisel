//
// Created by Giuseppe Francione on 05/10/25.
//

#include "mseed_encoder.hpp"
#include "../utils/logger.hpp"
#include <libmseed.h>
#include <string>
#include <vector>
#include <algorithm>
#include <limits>
#include <cmath>
#include <filesystem>
#include <fstream>

MseedEncoder::MseedEncoder(const bool preserve_metadata) {
    preserve_metadata_ = preserve_metadata;
}

// recompress input miniSEED file into output file
bool MseedEncoder::recompress(const std::filesystem::path &input,
                              const std::filesystem::path &output) {
    Logger::log(LogLevel::INFO, "Starting MiniSEED recompression", name());

    MS3Record *msr = nullptr;
    uint32_t read_flags = 0;
    constexpr int8_t verbose = 0;

    // validate crc and unpack data
    read_flags |= MSF_VALIDATECRC;
    read_flags |= MSF_UNPACKDATA;

    // first read with filename
    int retcode = ms3_readmsr(&msr, input.string().c_str(), read_flags, verbose);
    while (retcode == MS_NOERROR) {
        // empirically choose reclen
        const int reclen = choose_reclen(msr, msr->numsamples);

        uint32_t write_flags = MSF_FLUSHDATA;
        if (msr->formatversion == 2) {
            write_flags |= MSF_PACKVER2;
        }

        if (msr->sampletype == 'i') {
            msr->encoding = DE_STEIM2;
        }

        msr->reclen = reclen;

        // write record(s) to output
        const int64_t rv = msr3_writemseed(msr, output.string().c_str(), 1, write_flags, verbose);
        if (rv < 0) {
            Logger::log(LogLevel::ERROR, "Error writing MiniSEED record", name());
            ms3_readmsr(&msr, nullptr, read_flags, 0);
            return false;
        }

        // next read with null
        retcode = ms3_readmsr(&msr, nullptr, read_flags, verbose);
    }

    if (retcode != MS_ENDOFFILE) {
        Logger::log(LogLevel::ERROR, "Error reading MiniSEED: " + std::string(ms_errorstr(retcode)), name());
        ms3_readmsr(&msr, nullptr, read_flags, 0);
        return false;
    }

    ms3_readmsr(&msr, nullptr, read_flags, 0);

    Logger::log(LogLevel::INFO, "Recompression completed successfully", name());
    return true;
}

// brute-force: try multiple reclen values and choose the smallest output
int MseedEncoder::choose_reclen(const MS3Record *msr, const size_t sample_count) {
    if (!msr || sample_count == 0) return 4096;

    const int max_reclen_exp = (msr->formatversion == 2) ? 16 : 20;
    constexpr int MIN_RECLEN_EXP = 8;
    std::vector<int> candidates;
    for (int e = MIN_RECLEN_EXP; e <= max_reclen_exp; ++e)
        candidates.push_back(1 << e);

    const std::filesystem::path tmpdir = std::filesystem::temp_directory_path();
    uint64_t best_size = std::numeric_limits<uint64_t>::max();
    int best_reclen = 4096;

    for (const int& reclen : candidates) {
        MS3Record *clone = msr3_duplicate(msr, 1);
        if (!clone) continue;

        clone->reclen = reclen;
        if (clone->sampletype == 'i') {
            clone->encoding = DE_STEIM2;
        }

        std::filesystem::path tmpfile = tmpdir / ("mseed_test_" + std::to_string(reclen) + ".mseed");

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