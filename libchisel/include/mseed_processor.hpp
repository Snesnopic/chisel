//
// Created by Giuseppe Francione on 19/10/25.
//

#ifndef CHISEL_MSEED_PROCESSOR_HPP
#define CHISEL_MSEED_PROCESSOR_HPP

#include "processor.hpp"
#include <array>
#include <string_view>
#include <span>

struct MS3Record; // forward declaration

namespace chisel {

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
            static constexpr std::array<std::string_view,1> kExts = { ".mseed" };
            return {kExts.data(), kExts.size()};
        }

        [[nodiscard]] bool can_recompress() const noexcept override { return true; }
        [[nodiscard]] bool can_extract_contents() const noexcept override { return false; }

        void recompress(const std::filesystem::path& input,
                        const std::filesystem::path& output,
                        bool preserve_metadata) override;

        std::optional<ExtractedContent> prepare_extraction(
            [[maybe_unused]] const std::filesystem::path& input_path) override { return std::nullopt; }

        void finalize_extraction(const ExtractedContent&,
                                 [[maybe_unused]] ContainerFormat target_format) override {}

        [[nodiscard]] std::string get_raw_checksum(const std::filesystem::path& file_path) const override;

        [[nodiscard]] bool raw_equal(const std::filesystem::path &a, const std::filesystem::path &b) const override;

    private:
        static int choose_reclen(const MS3Record* msr, size_t sample_count);
    };

} // namespace chisel

#endif // CHISEL_MSEED_PROCESSOR_HPP