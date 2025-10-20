//
// Created by Giuseppe Francione on 19/10/25.
//

#ifndef CHISEL_PDF_PROCESSOR_HPP
#define CHISEL_PDF_PROCESSOR_HPP

#include "processor.hpp"
#include <array>
#include <string_view>
#include <span>
#include <unordered_map>
#include <filesystem>
#include <vector>

namespace chisel {

class PdfProcessor final : public IProcessor {
public:
    [[nodiscard]] std::string_view get_name() const noexcept override {
        return "PdfProcessor";
    }

    [[nodiscard]] std::span<const std::string_view> get_supported_mime_types() const noexcept override {
        static constexpr std::array<std::string_view,1> kMimes = { "application/pdf" };
        return {kMimes.data(), kMimes.size()};
    }

    [[nodiscard]] std::span<const std::string_view> get_supported_extensions() const noexcept override {
        static constexpr std::array<std::string_view,1> kExts = { ".pdf" };
        return {kExts.data(), kExts.size()};
    }

    [[nodiscard]] bool can_recompress() const noexcept override { return true; }
    [[nodiscard]] bool can_extract_contents() const noexcept override { return true; }

    void recompress(const std::filesystem::path&,
                    const std::filesystem::path&,
                    bool) override {
        // intentionally empty: PDF recompression is handled in finalize_extraction
    }

    std::optional<ExtractedContent> prepare_extraction(const std::filesystem::path& input_path) override;
    void finalize_extraction(const ExtractedContent& content,
                             ContainerFormat target_format) override;

    [[nodiscard]] std::string get_raw_checksum(const std::filesystem::path& file_path) const override;

private:
    struct StreamInfo {
        bool decodable = false;
        bool has_decode_parms = false;
        std::filesystem::path file;
    };

    struct PdfState {
        std::vector<StreamInfo> streams;
        std::filesystem::path temp_dir;
    };

    std::unordered_map<std::filesystem::path, PdfState> state_;

    static std::filesystem::path make_temp_dir_for(const std::filesystem::path& input);
    static void cleanup_temp_dir(const std::filesystem::path& dir);
};

} // namespace chisel

#endif // CHISEL_PDF_PROCESSOR_HPP