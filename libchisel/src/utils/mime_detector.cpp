//
// Created by Giuseppe Francione on 11/10/25.
//
#ifndef _WIN32
#include <magic.h>
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
#include <zlib.h>

#ifndef _WIN32
namespace {
    // thread-local instances to avoid reloading db and race conditions
    thread_local magic_t tl_magic_mime = nullptr;
    thread_local magic_t tl_magic_desc = nullptr;

    magic_t get_magic_mime() {
        if (tl_magic_mime != nullptr) return tl_magic_mime;

        tl_magic_mime = magic_open(MAGIC_MIME_TYPE | MAGIC_ERROR);
        if (tl_magic_mime == nullptr) return nullptr;

        // try default system db first
        if (magic_load(tl_magic_mime, nullptr) == 0) {
            return tl_magic_mime;
        }

        // fallback to extracted local db
        const auto target = chisel::MimeDetector::get_magic_file_path();
        if (magic_load(tl_magic_mime, target.c_str()) == 0) {
            return tl_magic_mime;
        }

        magic_close(tl_magic_mime);
        tl_magic_mime = nullptr;
        return nullptr;
    }

    magic_t get_magic_desc() {
        if (tl_magic_desc != nullptr) return tl_magic_desc;

        tl_magic_desc = magic_open(MAGIC_NONE | MAGIC_ERROR);
        if (tl_magic_desc == nullptr) return nullptr;

        if (magic_load(tl_magic_desc, nullptr) == 0) {
            return tl_magic_desc;
        }

        const auto target = chisel::MimeDetector::get_magic_file_path();
        if (magic_load(tl_magic_desc, target.c_str()) == 0) {
            return tl_magic_desc;
        }

        magic_close(tl_magic_desc);
        tl_magic_desc = nullptr;
        return nullptr;
    }
}
#endif

std::string chisel::MimeDetector::detect(const std::filesystem::path& path)
{
#ifndef _WIN32
    const magic_t magic = get_magic_mime();
    if (magic == nullptr) return {};

    const char* mime = magic_file(magic, path.string().c_str());
    return (mime != nullptr) ? mime : "";
#else
    auto ext = path.extension().string();
    std::ranges::transform(ext, ext.begin(), ::tolower);
    auto it = ext_to_mime.find(ext);
    return it != ext_to_mime.end() ? it->second : "application/octet-stream";
#endif
}

bool chisel::MimeDetector::is_mpeg1_layer3(const std::filesystem::path& path)
{
#ifndef _WIN32
    magic_t magic = get_magic_desc();
    if (magic == nullptr) return false;

    const char* desc = magic_file(magic, path.string().c_str());
    if (desc != nullptr)
    {
        std::string s(desc);
        if (s.find("MPEG") != std::string::npos &&
            s.find("layer III") != std::string::npos &&
            (s.find("v1") != std::string::npos || s.find("version 1") != std::string::npos))
        {
            return true;
        }
    }
    return false;
#else
    // fallback: only check .mp3 extension
    return path.extension() == ".mp3";
#endif
}

/**
 * @brief Decompresses a Gzip buffer into a vector of bytes.
 * @param data Pointer to the compressed Gzip data.
 * @param len The length of the compressed data.
 * @return A vector containing the decompressed data.
 * @throws std::runtime_error on zlib errors.
 */
std::vector<unsigned char> decompress_gzip(const unsigned char* data, const size_t len)
{
    std::vector<unsigned char> out;
    out.resize(8 * 1024 * 1024); // start with 8mb

    z_stream strm{};
    strm.next_in = const_cast<Bytef*>(data);
    strm.avail_in = static_cast<uInt>(len);

    if (inflateInit2(&strm, 16 + MAX_WBITS) != Z_OK)
    {
        throw std::runtime_error("inflateInit2 failed");
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

std::filesystem::path chisel::MimeDetector::get_magic_file_path()
{
#ifdef __APPLE__
    const char* home = getenv("HOME");
    return std::filesystem::path((home != nullptr) ? home : ".") /
        "Library/Application Support/chisel/magic.mgc";
#else
    const char* home = getenv("HOME");
    return std::filesystem::path(home ? home : ".") /
        ".local/share/chisel/magic.mgc";
#endif
}

void chisel::MimeDetector::ensure_magic_installed()
{
#ifndef _WIN32
    // test if system db works natively
    magic_t test_magic = magic_open(MAGIC_NONE);
    if (test_magic != nullptr) {
        if (magic_load(test_magic, nullptr) == 0) {
            magic_close(test_magic);
            return;
        }
        magic_close(test_magic);
    }

    const auto target = get_magic_file_path();
    bool need_install = !std::filesystem::exists(target);
    if (!need_install) {
        std::error_code ec;

        if (std::filesystem::file_size(target, ec) < 1024UL * 1024UL) {
             need_install = true;
             Logger::log(LogLevel::Warning, "Local magic database too small or suspicious, regenerating...", "MimeDetector");
        }
        else {
            magic_t test_load = magic_open(MAGIC_NONE);
            if (test_load != nullptr) {
                if (magic_load(test_load, target.c_str()) != 0) {
                    need_install = true;
                    Logger::log(LogLevel::Warning, "Local magic database corrupt (load failed), regenerating...", "MimeDetector");
                }
                magic_close(test_load);
            }
        }
    }

    if (need_install)
    {
        Logger::log(LogLevel::Info, "Installing embedded magic.mgc to " + target.string(), "MimeDetector");
        std::filesystem::create_directories(target.parent_path());

        const auto decompressed = decompress_gzip(embedded_magic_mgc, embedded_magic_mgc_len);
        std::ofstream ofs(target, std::ios::binary);
        ofs.write(reinterpret_cast<const char*>(decompressed.data()), static_cast<long>(decompressed.size()));
        ofs.close();
    }
#else
    // _putenv_s("MAGIC", target.string().c_str());
#endif
}
