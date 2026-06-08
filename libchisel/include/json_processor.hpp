//
// Created by Giuseppe Francione on 04/06/26.
//

/**
 * @file json_processor.hpp
 * @brief defines the IProcessor implementation for JSON files.
 */

#ifndef CHISEL_JSON_PROCESSOR_HPP
#define CHISEL_JSON_PROCESSOR_HPP

#include "processor.hpp"
#include <array>
#include <string_view>
#include <span>

namespace chisel {

/**
 * @brief implements IProcessor for JSON files using yyjson.
 *
 * @details parses JSON documents to perform minification (recompression).
 */
class JsonProcessor : public IProcessor {
public:
    // --- self-description ---

    [[nodiscard]] std::string_view get_name() const noexcept override {
        return "JsonProcessor";
    }

    [[nodiscard]] std::span<const std::string_view> get_supported_mime_types() const noexcept override {
        static constexpr std::array<std::string_view, 1> mimes = {
            "application/json"
        };
        return {mimes.data(), mimes.size()};
    }

    [[nodiscard]] std::span<const std::string_view> get_supported_extensions() const noexcept override {
        static constexpr std::array<std::string_view, 1> exts = {
            ".json"
        };
        return {exts.data(), exts.size()};
    }

    // --- capabilities ---

    [[nodiscard]] bool can_recompress() const noexcept override { return true; }
    [[nodiscard]] bool can_extract_contents() const noexcept override { return false; }

    // --- operations ---

    void recompress(const std::filesystem::path& input_path,
                    const std::filesystem::path& output_path, const ProcessingOptions &options) override;

    std::optional<ExtractedContent> prepare_extraction(const std::filesystem::path& input_path) override;

    std::filesystem::path finalize_extraction(const ExtractedContent& content, const ProcessingOptions &options) override;

    // --- integrity check ---

    [[nodiscard]] std::string get_raw_checksum(const std::filesystem::path& file_path) const override;

    [[nodiscard]] bool raw_equal(const std::filesystem::path& a,
                                 const std::filesystem::path& b) const override;
};

} // namespace chisel

#endif // CHISEL_JSON_PROCESSOR_HPP
