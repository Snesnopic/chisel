//
// Created by Giuseppe Francione on 11/06/26.
//

/**
 * @file lua_processor.hpp
 * @brief Defines the IProcessor implementation for compiled Lua bytecode files.
 */

#ifndef CHISEL_LUA_PROCESSOR_HPP
#define CHISEL_LUA_PROCESSOR_HPP

#include "processor.hpp"
#include <array>
#include <string_view>
#include <span>

namespace chisel {

/**
 * @brief Implements IProcessor for compiled Lua bytecode files (.lua, .luac).
 *
 * @details Strips debug information (source name, line numbers, local variable
 * names, upvalue names) from Lua 5.1, 5.2, and 5.3 compiled bytecode.
 * The resulting bytecode is functionally identical — all opcodes, constants
 * and nested prototypes are preserved verbatim.
 */
class LuaProcessor final : public IProcessor {
public:
    // --- self-description ---

    [[nodiscard]] std::string_view get_name() const noexcept override {
        return "LuaProcessor";
    }

    [[nodiscard]] std::span<const std::string_view> get_supported_mime_types() const noexcept override {
        static constexpr std::array<std::string_view, 1> kMimes = {
            "application/x-lua-bytecode"
        };
        return {kMimes.data(), kMimes.size()};
    }

    [[nodiscard]] std::span<const std::string_view> get_supported_extensions() const noexcept override {
        static constexpr std::array<std::string_view, 2> kExts = { ".lua", ".luac" };
        return {kExts.data(), kExts.size()};
    }

    // --- capabilities ---

    [[nodiscard]] bool can_recompress() const noexcept override { return true; }
    [[nodiscard]] bool can_extract_contents() const noexcept override { return false; }

    // --- operations ---

    void recompress(const std::filesystem::path& input_path,
                    const std::filesystem::path& output_path,
                    const ProcessingOptions& options) override;

    std::optional<ExtractedContent> prepare_extraction(const std::filesystem::path&) override {
        return std::nullopt;
    }

    std::filesystem::path finalize_extraction(const ExtractedContent&, const ProcessingOptions&) override {
        return {};
    }

    // --- integrity check ---

    [[nodiscard]] std::string get_raw_checksum(const std::filesystem::path& /*file_path*/) const override {
        return "";
    }

    [[nodiscard]] bool raw_equal(const std::filesystem::path& a,
                                 const std::filesystem::path& b) const override;
};

} // namespace chisel

#endif // CHISEL_LUA_PROCESSOR_HPP
