//
// Created by Giuseppe Francione on 13/11/25.
//

#ifndef CHISEL_FILE_UTILS_HPP
#define CHISEL_FILE_UTILS_HPP

#include <cstdio>
#include <filesystem>
#include <string>

namespace chisel {

    /**
     * @brief Opens a file using a filesystem path, handling Windows Unicode correctly.
     * @param path The path to the file.
     * @param mode The standard C fopen mode string (e.g., "rb", "wb").
     * @return FILE* pointer or nullptr if open failed.
     */
    inline FILE* open_file(const std::filesystem::path& path, const char* mode) {
#ifdef _WIN32
        // On Windows, convert mode to wstring and use _wfopen, which accepts
        // wide-char paths (UTF-16), supporting Unicode and long paths.
        std::wstring wmode;
        for (const char* p = mode; *p; ++p) wmode += static_cast<wchar_t>(*p);
        return _wfopen(path.wstring().c_str(), wmode.c_str());
#else
        return std::fopen(path.string().c_str(), mode);
#endif
    }

} // namespace chisel

#endif // CHISEL_FILE_UTILS_HPP