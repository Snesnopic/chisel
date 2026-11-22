#ifndef CHISEL_APP_PATHS_HPP
#define CHISEL_APP_PATHS_HPP

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace chisel_app {

    // utf-8 on every platform, whatever the system code page
    std::string to_utf8(const std::filesystem::path& path);
    std::filesystem::path from_utf8(std::string_view text);

    // absolute and lexically normal, without a trailing separator
    std::filesystem::path normalized_path(const std::filesystem::path& path);

    // lowercase extension without the dot, empty when there is none
    std::string lower_extension(const std::filesystem::path& path);

    // size and modification time, to tell whether something else changed a file
    struct FileStamp {
        std::uintmax_t size = 0;
        std::filesystem::file_time_type mtime{};
        bool operator==(const FileStamp&) const = default;
    };
    std::optional<FileStamp> stamp_of(const std::filesystem::path& file);

    // what tells files on different volumes apart: the volume mount point on windows, the device elsewhere
    std::string volume_key(const std::filesystem::path& path);

    std::filesystem::path common_parent_of(const std::filesystem::path& a, const std::filesystem::path& b);

    // deepest folder holding every path, a folder counting as itself; empty when there is none (different drives)
    template <typename Range>
    std::filesystem::path common_parent(const Range& paths) {
        std::optional<std::filesystem::path> common;
        for (const auto& path : paths) {
            std::error_code ec;
            auto dir = normalized_path(path);
            if (!std::filesystem::is_directory(dir, ec)) dir = dir.parent_path();
            common = common ? common_parent_of(*common, dir) : dir;
        }
        return common.value_or(std::filesystem::path{});
    }

} // namespace chisel_app

#endif // CHISEL_APP_PATHS_HPP
