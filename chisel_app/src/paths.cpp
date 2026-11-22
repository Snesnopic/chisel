#include "chisel_app/paths.hpp"

#include <algorithm>
#include <cctype>

#ifdef _WIN32
#define NOGDI
#define NOUSER
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;

namespace chisel_app {

    std::string to_utf8(const fs::path& path) {
        const auto text = path.u8string();
        return {reinterpret_cast<const char*>(text.data()), text.size()};
    }

    fs::path from_utf8(const std::string_view text) {
        return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
    }

    fs::path normalized_path(const fs::path& path) {
        std::error_code ec;
        auto result = fs::absolute(path, ec);
        if (ec) result = path;
        result = result.lexically_normal();
        if (!result.has_filename() && result.has_relative_path()) result = result.parent_path();
        return result;
    }

    std::string lower_extension(const fs::path& path) {
        auto ext = to_utf8(path.extension());
        if (!ext.empty() && ext.front() == '.') ext.erase(ext.begin());
        std::ranges::transform(ext, ext.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return ext;
    }

    std::optional<FileStamp> stamp_of(const fs::path& file) {
        std::error_code ec;
        const auto size = fs::file_size(file, ec);
        if (ec) return std::nullopt;
        const auto mtime = fs::last_write_time(file, ec);
        if (ec) return std::nullopt;
        return FileStamp{.size = size, .mtime = mtime};
    }

    std::string volume_key(const fs::path& path) {
        const auto dir = normalized_path(path).parent_path();
#ifdef _WIN32
        wchar_t volume[MAX_PATH + 1] = {};
        if (GetVolumePathNameW(dir.c_str(), volume, MAX_PATH + 1)) {
            return to_utf8(fs::path(volume));
        }
        return to_utf8(dir.root_name());
#else
        struct stat st{};
        if (::stat(dir.c_str(), &st) == 0) return std::to_string(static_cast<unsigned long long>(st.st_dev));
        return {};
#endif
    }

    fs::path common_parent_of(const fs::path& a, const fs::path& b) {
        fs::path prefix;
        for (auto x = a.begin(), y = b.begin(); x != a.end() && y != b.end() && *x == *y; ++x, ++y) {
            prefix /= *x;
        }
        return prefix;
    }

} // namespace chisel_app
