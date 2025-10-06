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
    int retcode;

    // validate crc and unpack data
    read_flags |= MSF_VALIDATECRC;
    read_flags |= MSF_UNPACKDATA;

    // loop over input file record by record
    while ((retcode = ms3_readmsr(&msr, input.string().c_str(), read_flags, verbose)) == MS_NOERROR) {
        // choose reclen
        const int reclen = choose_reclen(msr, msr->numsamples);

        // keep same output format version as input
        uint32_t write_flags = MSF_FLUSHDATA;
        if (msr->formatversion == 2) {
            write_flags |= MSF_PACKVER2;
        }

        // force steim2 for integer samples
        if (msr->sampletype == 'i') {
            msr->encoding = DE_STEIM2;
        }

        msr->reclen = reclen;

        // write record(s) to output
        const int64_t rv = msr3_writemseed(msr, output.string().c_str(), 1, write_flags, verbose);
        if (rv < 0) {
            Logger::log(LogLevel::ERROR, "Error writing MiniSEED record", name());
            ms3_readmsr(&msr, nullptr, read_flags, 0); // cleanup
            return false;
        }
    }

    if (retcode != MS_ENDOFFILE) {
        Logger::log(LogLevel::ERROR, "Error reading MiniSEED: " + std::string(ms_errorstr(retcode)), name());
        ms3_readmsr(&msr, nullptr, read_flags, 0);
        return false;
    }

    // cleanup
    ms3_readmsr(&msr, nullptr, read_flags, 0);

    Logger::log(LogLevel::INFO, "Recompression completed successfully", name());
    return true;
}

int MseedEncoder::choose_reclen(const MS3Record *msr, const size_t sample_count) {
    int max_reclen_exp = 20;

    if ( msr->formatversion == 2 ) {
        max_reclen_exp = 16;
    }
    constexpr int MIN_RECLEN_EXP = 8;   // 2^8 = 256
    constexpr int DEFAULT_RECLEN = 4096; // fallback if input is unusable
    // defensive checks
    if (sample_count == 0) return DEFAULT_RECLEN;

    // build candidate powers of two
    std::vector<int> candidates;
    for (int e = MIN_RECLEN_EXP; e <= max_reclen_exp; ++e)
        candidates.push_back(1 << e);

    // observed values (use msr when available)
    const uint64_t input_reclen = (msr && msr->reclen > 0) ? static_cast<uint64_t>(msr->reclen) : static_cast<uint64_t>(DEFAULT_RECLEN);
    const int64_t in_numsamples = (msr && msr->numsamples > 0) ? static_cast<int64_t>(msr->numsamples) : -1;
    double observed_bps = -1.0; // bytes per sample (empirical)
    if (in_numsamples > 0) {
        observed_bps = static_cast<double>(input_reclen) / static_cast<double>(in_numsamples);
        // protect against degenerate values
        if (observed_bps < 0.1) observed_bps = 0.1;
    } else {
        // fallback estimate: assume 4 bytes per sample (conservative)
        observed_bps = 4.0;
    }

    // rough estimate of header overhead inside a record (bytes)
    // choose a conservative estimate so we don't overpack records
    constexpr double header_estimate = 128.0;

    // small empirical gain factor: larger records may compress slightly better
    // keep this modest to avoid overly favoring huge records
    constexpr double max_compression_gain = 0.05; // up to 5% bps reduction

    // reference reclen used to scale compression gain (input reclen)
    const double ref_reclen = static_cast<double>(input_reclen);

    double best_cost = std::numeric_limits<double>::infinity();
    int best_reclen = candidates.front(); // default to smallest candidate

    for (const int reclen : candidates) {
        // compute a small, bounded gain if reclen > ref_reclen
        double reclen_ratio = static_cast<double>(reclen) / ref_reclen;
        double gain = 0.0;
        if (reclen_ratio > 1.0) {
            // use log2 spacing for diminishing returns
            double max_span = static_cast<double>((1 << (max_reclen_exp - MIN_RECLEN_EXP + 1)));
            double factor = std::log2(reclen_ratio) / std::log2(std::max(2.0, max_span));
            factor = std::clamp(factor, 0.0, 1.0);
            gain = max_compression_gain * factor;
        }

        // estimate bytes-per-sample after gain
        const double est_bps = std::max(0.5, observed_bps * (1.0 - gain));

        // usable payload bytes in this record
        double usable_bytes = static_cast<double>(reclen) - header_estimate;
        if (usable_bytes < 32.0) {
            // skip unrealistic tiny payloads
            continue;
        }

        // how many samples fit in one record of this reclen
        int samples_per_record = static_cast<int>(std::floor(usable_bytes / est_bps));
        if (samples_per_record < 1) samples_per_record = 1;

        // how many records needed to store sample_count
        const double records_needed = std::ceil(static_cast<double>(sample_count) / static_cast<double>(samples_per_record));
        double total_bytes = records_needed * static_cast<double>(reclen);

        // penalize excessively long record durations (avoid hours-long records)
        if (msr && msr->samprate > 0.0) {
            double duration_sec = static_cast<double>(samples_per_record) / msr->samprate;
            if (duration_sec > 3600.0) {
                // add a mild penalty proportional to log2 of how many hours
                double hours_factor = std::log2(duration_sec / 3600.0);
                if (hours_factor > 0.0) total_bytes *= (1.0 + 0.05 * hours_factor);
            }
        }

        // pick the reclen that minimizes expected total bytes on disk
        if (total_bytes < best_cost) {
            best_cost = total_bytes;
            best_reclen = reclen;
        }
    }

    // final sanity: ensure chosen reclen is in candidates
    if (std::find(candidates.begin(), candidates.end(), best_reclen) == candidates.end())
        return DEFAULT_RECLEN;

    return best_reclen;
}