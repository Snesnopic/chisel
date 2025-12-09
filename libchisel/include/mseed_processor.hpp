//
// Created by Giuseppe Francione on 19/10/25.
//

/**
 * @file mseed_processor.hpp
 * @brief Defines the IProcessor implementation for MiniSEED seismic data files.
 */

#ifndef CHISEL_MSEED_PROCESSOR_HPP
#define CHISEL_MSEED_PROCESSOR_HPP

#include "processor.hpp"
#include <array>
#include <string_view>
#include <span>

struct MS3Record; // forward declaration

namespace chisel {

    /**
     * @brief Implements IProcessor for MiniSEED files (.mseed) using libmseed.
     *
     * @details This processor performs a full read-unpack-repack cycle.
     * It reads all trace data from the input file and re-packs it
     * into a new file, applying optimal encodings (like Steim2 for
     * integers) and record lengths.
     */
    class MseedProcessor final : public IProcessor {
    public:
        [[nodiscard]] std::string_view get_name() const noexcept override {
            return "MseedProcessor";
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_mime_types() const noexcept override {
            static constexpr std::array<std::string_view,1> kMimes = { "application/vnd.fdsn.mseed" };
            return {kMimes.data(), kMimes.size()};
        }

        [[nodiscard]] std::span<const std::string_view> get_supported_extensions() const noexcept override {
            static constexpr std::array<std::string_view,3> kExts = { ".mseed", ".mseed2", ".mseed3" };
            return {kExts.data(), kExts.size()};
        }

        [[nodiscard]] bool can_recompress() const noexcept override { return true; }
        [[nodiscard]] bool can_extract_contents() const noexcept override { return false; }

        /**
         * @brief Recompresses a MiniSEED file using libmseed.
         *
         * Reads the full trace list, unpacks data samples, and then
         * re-packs the data using `mstl3_pack_segment`. It attempts
         * to use optimal encodings like Steim2 and falls back to
         * uncompressed formats (like 32-bit int) if packing fails.
         *
         * @param input Path to the source MiniSEED file.
         * @param output Path to write the optimized MiniSEED file.
         * @param preserve_metadata (Ignored) Metadata is intrinsic
         * to the MiniSEED record structure and is always preserved.
         * @throws std::runtime_error if libmseed fails to read or write.
         */
        void recompress(const std::filesystem::path& input,
                        const std::filesystem::path& output,
                        bool preserve_metadata) override;

        /**
         * @brief MiniSEED is not a container format.
         * @return std::nullopt
         */
        std::optional<ExtractedContent> prepare_extraction(
            [[maybe_unused]] const std::filesystem::path& input_path) override { return std::nullopt; }

        /**
         * @brief MiniSEED is not a container format.
         * @return Empty path.
         */
        std::filesystem::path finalize_extraction(const ExtractedContent &) override {return {};}

        /**
         * @brief (Not Implemented) Compute a raw checksum.
         * @param file_path Path to the file.
         * @return An empty string.
         */
        [[nodiscard]] std::string get_raw_checksum(const std::filesystem::path& file_path) const override;

        /**
         * @brief Compares two MiniSEED files by decoding all traces and samples.
         *
         * This performs a deep, sample-level comparison. It unpacks
         * both files and compares trace IDs, segment counts, start times,
         * sample counts, sample types, sample rates, and finally
         * the raw sample data itself (with float tolerance).
         *
         * @param a First MiniSEED file.
         * @param b Second MiniSEED file.
         * @return true if all traces and sample data are identical.
         */
        [[nodiscard]] bool raw_equal(const std::filesystem::path &a, const std::filesystem::path &b) const override;

    private:
        /**
         * @brief Internal helper to determine an optimal record length.
         *
         * Logic is based on the original format version, sample type,
         * and sample count to select a power-of-2 record length.
         *
         * @param original_version The MiniSEED format version (2 or 3).
         * @param sampleType The libmseed sample type char (e.g., 'i', 'f', 'd').
         * @param sample_count The number of samples in the record.
         * @return An appropriate record length (e.g., 256, 4096).
         */
        static int choose_reclen(uint8_t original_version, char sampleType,  int64_t sample_count);
    };

} // namespace chisel

#endif // CHISEL_MSEED_PROCESSOR_HPP