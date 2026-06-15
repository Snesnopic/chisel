#ifndef CHISEL_MIME_PROCESSOR_HPP
#define CHISEL_MIME_PROCESSOR_HPP

#include "processor.hpp"
#include <memory>
#include <vector>
#include <string>
#include <variant>
#include <array>

namespace chisel {

struct TextChunk {
    std::string text;
};

struct Base64Chunk {
    std::size_t file_index;
};

using MimeChunk = std::variant<TextChunk, Base64Chunk>;

struct MimeState {
    std::vector<MimeChunk> chunks;
};

class MimeProcessor : public IProcessor {
public:
    [[nodiscard]] std::string_view get_name() const noexcept override {
        return "MimeProcessor";
    }

    [[nodiscard]] std::span<const std::string_view, std::dynamic_extent>
    get_supported_mime_types() const noexcept override {
        static constexpr std::array<std::string_view, 4> mimes = {
            "message/rfc822",
            "application/vnd.ms-outlook",
            "multipart/related",
            "message/x-mixed-replace"
        };
        return {mimes};
    }

    [[nodiscard]] std::span<const std::string_view, std::dynamic_extent>
    get_supported_extensions() const noexcept override {
        static constexpr std::array<std::string_view, 5> exts = {
            ".eml", ".msg", ".mht", ".mhtml", ".mbx"
        };
        return {exts};
    }

    [[nodiscard]] bool can_recompress() const noexcept override { return false; }
    [[nodiscard]] bool can_extract_contents() const noexcept override { return true; }

    void recompress(const std::filesystem::path& input_path,
                    const std::filesystem::path& output_path, const ProcessingOptions &options) override {}

    std::optional<ExtractedContent> prepare_extraction(
        const std::filesystem::path& input_path) override;

    std::filesystem::path finalize_extraction(const ExtractedContent& content, const ProcessingOptions &options) override;

    [[nodiscard]] std::string get_raw_checksum(const std::filesystem::path& file_path) const override;

    [[nodiscard]] bool raw_equal(const std::filesystem::path& a,
                                 const std::filesystem::path& b) const override;
};

} // namespace chisel

#endif // CHISEL_MIME_PROCESSOR_HPP
