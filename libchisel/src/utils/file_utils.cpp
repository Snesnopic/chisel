//
// Created by Giuseppe Francione on 17/11/25.
//

#include <filesystem>
#include "../../include/file_utils.hpp"
#include "../../include/logger.hpp"
#include "../../include/random_utils.hpp"
#include <system_error>

namespace chisel {

    FILE* open_file(const std::filesystem::path& path, const char* mode) {
#ifdef _WIN32
        // On Windows, convert mode to wstring and use _wfopen, which accepts
        // wide-char paths (UTF-16), supporting Unicode and long paths.
        std::wstring wmode;
        for (const char* p = mode; *p; ++p) wmode += static_cast<wchar_t>(*p);

        // get absolute path, required for the long path prefix
        std::error_code ec;
        auto abs_path = std::filesystem::absolute(path, ec);
        if (ec) {
            // fallback to original behavior on error
            return _wfopen(path.wstring().c_str(), wmode.c_str());
        }

        // prepend the magic prefix to bypass MAX_PATH
        std::wstring long_path = L"\\\\?\\" + abs_path.wstring();
        return _wfopen(long_path.c_str(), wmode.c_str());
#else
        return std::fopen(path.string().c_str(), mode);
#endif
    }
    std::filesystem::path make_temp_dir_for(const std::filesystem::path& input_path, const std::string& prefix) {
        // use a common base dir inside temp
        const auto base_tmp = std::filesystem::temp_directory_path() /
            ("chisel-" + prefix);

        std::error_code ec;
        std::filesystem::create_directories(base_tmp, ec);

        const std::string stem = input_path.stem().string();
        const std::string dir_name = prefix + "_" + stem + "_" + RandomUtils::random_suffix();
        auto dir = base_tmp / dir_name;

        std::filesystem::create_directories(dir, ec);
        if (ec) {
            Logger::log(LogLevel::Error,
                "Failed to create temp dir: " + dir.string() + " (" + ec.message() + ")",
                "file_utils");
        }
        return dir;
    }

    void cleanup_temp_dir(const std::filesystem::path& dir, const std::string_view tag) {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        if (ec) {
            Logger::log(LogLevel::Warning, "Can't remove temp dir: " + dir.string() + " (" + ec.message() + ")", tag);
        } else {
            Logger::log(LogLevel::Debug, "Removed temp dir: " + dir.string(), tag);
        }
    }

} // namespace chisel

