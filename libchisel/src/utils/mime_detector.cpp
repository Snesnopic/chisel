//
// Created by Giuseppe Francione on 11/10/25.
//

#include <magic.h>
#ifdef _WIN32
#include "magic_mgc_windows.h"
#else
#include "magic_mgc.h"
#endif
#include "../../include/mime_detector.hpp"
#include "../../include/file_type.hpp"
#include "../../include/logger.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <vector>
#include <string>
#include <stdexcept>
#include <mutex>
#include <zlib.h>


namespace chisel {

namespace {
    // extract gzip payload to memory
    std::vector<unsigned char> decompress_gzip(const unsigned char* data, const size_t len)
    {
        std::vector<unsigned char> out;
        out.resize(8ULL * 1024ULL * 1024ULL);

        z_stream strm{};
        strm.next_in = const_cast<Bytef*>(data);
        strm.avail_in = static_cast<uInt>(len);

        if (inflateInit2(&strm, 16 + MAX_WBITS) != Z_OK)
        {
            throw std::runtime_error("inflateinit2 failed");
        }

        int ret;
        do
        {
            if (strm.total_out >= out.size())
            {
                out.resize(out.size() * 2);
            }
            strm.next_out = out.data() + strm.total_out;
            strm.avail_out = out.size() - strm.total_out;

            ret = inflate(&strm, Z_NO_FLUSH);
            if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR)
            {
                inflateEnd(&strm);
                throw std::runtime_error("inflate failed");
            }
        }
        while (ret != Z_STREAM_END);

        const size_t out_size = strm.total_out;
        inflateEnd(&strm);
        out.resize(out_size);
        return out;
    }

    // thread local wrapper to prevent reloading magic db on every call
    struct magic_handle {
        magic_t handle;

        explicit magic_handle(int flags) {
            handle = magic_open(flags | MAGIC_ERROR);
            if (handle != nullptr) {
                magic_load(handle, nullptr);
            }
        }

        ~magic_handle() {
            if (handle != nullptr) {
                magic_close(handle);
            }
        }
    };

    magic_t get_magic_mime() {
        thread_local magic_handle mh(MAGIC_MIME_TYPE);
        return mh.handle;
    }

    magic_t get_magic_desc() {
        thread_local magic_handle mh(MAGIC_NONE);
        return mh.handle;
    }
} // namespace

std::string MimeDetector::detect(const std::filesystem::path& path)
{
    const magic_t magic = get_magic_mime();
    if (magic == nullptr) return "application/octet-stream";

    const char* mime = magic_file(magic, path.string().c_str());
    return mime != nullptr ? mime : "application/octet-stream";
}


    std::filesystem::path MimeDetector::get_magic_file_path()
{
#ifdef _WIN32
    const char* appdata = getenv("LOCALAPPDATA");
    return std::filesystem::path(appdata != nullptr ? appdata : ".") /
        "chisel"/"magic.mgc";
#elif defined(__APPLE__)
    const char* home = getenv("HOME");
    return std::filesystem::path((home != nullptr) ? home : ".") /
        "Library" /"Application Support"/"chisel"/"magic.mgc";
#else
    const char* home = getenv("HOME");
    return std::filesystem::path(home != nullptr ? home : ".") /
        ".local"/"share"/"chisel"/"magic.mgc";
#endif
}
    void MimeDetector::ensure_magic_installed()
{
    static std::once_flag init_flag;
    std::call_once(init_flag, []() {
        const auto target = get_magic_file_path();
        bool need_install = !std::filesystem::exists(target);

        if (!need_install) {
            std::error_code ec;
            if (std::filesystem::file_size(target, ec) < 1024UL * 1024UL) {
                 need_install = true;
                 Logger::log(LogLevel::Warning, "LOCAL MAGIC DATABASE TOO SMALL, REGENERATING...", "MimeDetector");
            }
            else {
                magic_t test_magic = magic_open(MAGIC_NONE);
                if (test_magic != nullptr) {
                    if (magic_load(test_magic, target.string().c_str()) != 0) {
                        need_install = true;
                        Logger::log(LogLevel::Warning, "LOCAL MAGIC DATABASE CORRUPT, REGENERATING...", "MimeDetector");
                    }
                    magic_close(test_magic);
                }
            }
        }

        if (need_install)
        {
            Logger::log(LogLevel::Info, "INSTALLING EMBEDDED MAGIC.MGC TO " + target.string(), "MimeDetector");
            std::filesystem::create_directories(target.parent_path());

            const auto decompressed = decompress_gzip(embedded_magic_mgc, embedded_magic_mgc_len);
            std::ofstream ofs(target, std::ios::binary);
            ofs.write(reinterpret_cast<const char*>(decompressed.data()), static_cast<std::streamsize>(decompressed.size()));
        }

#ifdef _WIN32
        _putenv_s("MAGIC", target.string().c_str());
#else
        setenv("MAGIC", target.c_str(), 1);
#endif
    });
}
}