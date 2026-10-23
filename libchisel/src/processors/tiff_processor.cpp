//
// Created by Giuseppe Francione on 19/10/25.
//

#include "../../include/tiff_processor.hpp"
#include "../../include/c2pa_manifest.hpp"
#include "../../include/logger.hpp"
#include "../../include/file_utils.hpp"
#include "../../include/tiff_rewriter.hpp"
#include <vector>
#include <stdexcept>


namespace chisel {

void TiffProcessor::recompress(const std::filesystem::path& input,
                               const std::filesystem::path& output, const ProcessingOptions &options) {
    Logger::log(LogLevel::Debug, "Entering recompress for " + input.string(), get_name());

    if (const TiffRewriteResult result = rewrite_tiff(input, output, options); !result.written) {
        if (result.mismatch) {
            Logger::log(LogLevel::Error, "rewritten TIFF differs, file left as it is: " + input.string() + " - " + result.reason, get_name());
        } else {
            Logger::log(LogLevel::Debug, "TIFF left as it is: " + input.filename().string() + " - " + result.reason, get_name());
        }
        std::vector<uint8_t> data;
        if (!read_file(input, data) || !write_file(output, data))
            throw std::runtime_error("TiffProcessor: failed to copy input to output");
        return;
    }

    Logger::log(LogLevel::Debug, "Exiting recompress for " + output.string(), get_name());
}

std::string TiffProcessor::get_raw_checksum(const std::filesystem::path&) const {
    // TODO: implement checksum of raw TIFF data
    return "";
}
bool TiffProcessor::raw_equal(const std::filesystem::path& a, const std::filesystem::path& b) const {
    const std::string diff = compare_tiff(a, b, false);
    if (!diff.empty()) Logger::log(LogLevel::Debug, "Raw_equal: " + diff, get_name());
    return diff.empty();
}

bool TiffProcessor::is_signed(const std::filesystem::path& file_path) const {
    return c2pa::tiff_has_manifest(file_path);
}

} // namespace chisel
