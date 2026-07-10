//
// Created by Giuseppe Francione on 20/05/2026.
//

#include "../../include/kanzi_processor.hpp"
#include "../../include/logger.hpp"
#include "../../include/file_utils.hpp"
#include "../../include/random_utils.hpp"
#include "../../include/file_type.hpp"
#include <Context.hpp>
#include <array>
#include "app/BlockCompressor.hpp"
#include "app/BlockDecompressor.hpp"


namespace chisel {
    

std::optional<ExtractedContent> KanziProcessor::prepare_extraction(const std::filesystem::path& input_path) {
    Logger::log(LogLevel::Debug, "Starting kanzi decompression", get_name());

    ExtractedContent content;
    content.original_path = input_path;
    content.temp_dir = make_temp_dir_for(input_path, "kanzi");

    content.format = ContainerFormat::Kanzi;

    std::filesystem::path raw_bin = content.temp_dir / "stream.raw";

    try {
        kanzi::Context ctx;
        ctx.putString("inputName", input_path.string());
        ctx.putString("outputName", raw_bin.string());
        ctx.putInt("jobs", 1);
        ctx.putInt("overwrite", 1);
        ctx.putInt("verbosity", 0);

        kanzi::BlockDecompressor bd(ctx);
        uint64_t read = 0;

        if (bd.decompress(read) != 0) {
            Logger::log(LogLevel::Error, "Kanzi decompression returned non-zero status", get_name());
            return std::nullopt;
        }
    } catch (const std::exception& e) {
        Logger::log(LogLevel::Error, std::string("Kanzi exception: ") + e.what(), get_name());
        return std::nullopt;
    }

    // expose the uncompressed stream to the processing pipeline
    content.extracted_files.push_back(raw_bin);
    return content;
}

std::filesystem::path KanziProcessor::finalize_extraction(const ExtractedContent& content, const ProcessingOptions& /*options*/) {
    if (content.extracted_files.empty()) {
        Logger::log(LogLevel::Error, "No raw stream found for kanzi repacking", get_name());
        throw std::runtime_error("KanziProcessor: empty extracted files");
    }

    Logger::log(LogLevel::Debug, "Repacking kanzi", get_name());

    const std::filesystem::path& processed_bin = content.extracted_files.front();
    std::filesystem::path out_knz = std::filesystem::temp_directory_path() /
        (content.original_path.stem().string() + "_tmp" + RandomUtils::random_suffix() + ".knz");

    try {
        kanzi::Context ctx;
        ctx.putString("inputName", processed_bin.string());
        ctx.putString("outputName", out_knz.string());

        // force extreme compression (level 9 maps to EXE+RLT+TEXT+UTF+DNA&TPAQX)
        ctx.putInt("level", 9);
        ctx.putInt("autoBlock", 1);
        ctx.putInt("jobs", 1);
        ctx.putInt("overwrite", 1);
        ctx.putInt("checksum", 64); // enforce integrity

        kanzi::BlockCompressor bc(ctx);
        uint64_t written = 0;

        if (bc.compress(written) != 0) {
            Logger::log(LogLevel::Error, "Kanzi compression returned non-zero status", get_name());
            std::filesystem::remove(out_knz);
            throw std::runtime_error("KanziProcessor: compression failed");
        }
    } catch (const std::exception& e) {
        Logger::log(LogLevel::Error, std::string("Kanzi exception: ") + e.what(), get_name());
        std::filesystem::remove(out_knz);
        throw;
    }

    cleanup_temp_dir(content.temp_dir, get_name());
    return out_knz;
}

std::string KanziProcessor::get_raw_checksum(const std::filesystem::path& /*file_path*/) const {
    return "";
}

namespace {

std::vector<uint8_t> decode_kanzi(const std::filesystem::path& path) {
    const std::filesystem::path temp_dir = make_temp_dir_for(path, "kanzi_rawequal");
    const std::filesystem::path raw_bin = temp_dir / "stream.raw";

    kanzi::Context ctx;
    ctx.putString("inputName", path.string());
    ctx.putString("outputName", raw_bin.string());
    ctx.putInt("jobs", 1);
    ctx.putInt("overwrite", 1);
    ctx.putInt("verbosity", 0);

    kanzi::BlockDecompressor bd(ctx);
    uint64_t read = 0;

    if (bd.decompress(read) != 0) {
        cleanup_temp_dir(temp_dir);
        throw std::runtime_error("KanziProcessor: decompression failed for raw_equal");
    }

    auto data = read_file(raw_bin);
    cleanup_temp_dir(temp_dir);
    return data;
}

} // namespace

bool KanziProcessor::raw_equal(const std::filesystem::path& a, const std::filesystem::path& b) const {
    try {
        return decode_kanzi(a) == decode_kanzi(b);
    } catch (const std::exception& e) {
        Logger::log(LogLevel::Error, std::string("raw_equal failed: ") + e.what(), get_name());
        return false;
    }
}

} // namespace chisel