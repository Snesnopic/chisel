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
#include <zlib.h>

std::string chisel::MimeDetector::detect(const std::filesystem::path& path)
{
#ifndef _WIN32
    const magic_t magic = magic_open(MAGIC_MIME_TYPE | MAGIC_ERROR);
    if (!magic) return {};
    if (magic_load(magic, nullptr) != 0)
    {
        magic_close(magic);
        return {};
    }
    const char* mime = magic_file(magic, path.string().c_str());
    std::string result = mime ? mime : "";
    magic_close(magic);
    return result;
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
    const magic_t magic = magic_open(MAGIC_MIME_TYPE | MAGIC_ERROR);
    if (!magic) return false;
    if (magic_load(magic, nullptr) != 0)
    {
        magic_close(magic);
        return false;
    }
    const char* desc = magic_file(magic, path.string().c_str());
    bool ok = false;
    if (desc)
    {
        std::string s(desc);
        if (s.find("MPEG") != std::string::npos &&
            s.find("layer III") != std::string::npos &&
            (s.find("v1") != std::string::npos || s.find("version 1") != std::string::npos))
        {
            ok = true;
        }
    }
    magic_close(magic);
    return ok;
#else
    // fallback: only check .mp3 extension
    return path.extension() == ".mp3";
#endif
}

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
    return std::filesystem::path(home ? home : ".") /
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
    const auto target = get_magic_file_path();
    if (!std::filesystem::exists(target))
    {
        Logger::log(LogLevel::Info, "Installing embedded magic.mgc to " + target.string(), "libmagic");
        std::filesystem::create_directories(target.parent_path());

        const auto decompressed = decompress_gzip(embedded_magic_mgc, embedded_magic_mgc_len);
        std::ofstream ofs(target, std::ios::binary);
        ofs.write(reinterpret_cast<const char*>(decompressed.data()), static_cast<long>(decompressed.size()));
        ofs.close();
    }
    setenv("MAGIC", target.c_str(), 1);
#else
    // _putenv_s("MAGIC", target.string().c_str());
#endif
}