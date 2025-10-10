//
// Created by Giuseppe Francione on 18/09/25.
//

#include "file_type.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <magic.h>
#include "logger.hpp"
#include <cstdlib>
#include <vector>
#include <zlib.h>
#include "magic.mgc.h"

namespace {
    // decompress gzip buffer (from zopfli) using zlib, with dynamic growth
    std::vector<unsigned char> decompress_gzip(const unsigned char* data, size_t len) {
        std::vector<unsigned char> out;
        out.resize(8 * 1024 * 1024); // start with 8mb

        z_stream strm{};
        strm.next_in = const_cast<Bytef*>(data);
        strm.avail_in = static_cast<uInt>(len);

        if (inflateInit2(&strm, 16 + MAX_WBITS) != Z_OK) {
            throw std::runtime_error("inflateInit2 failed");
        }

        int ret;
        do {
            if (strm.total_out >= out.size()) {
                out.resize(out.size() * 2);
            }
            strm.next_out = out.data() + strm.total_out;
            strm.avail_out = out.size() - strm.total_out;

            ret = inflate(&strm, Z_NO_FLUSH);
            if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
                inflateEnd(&strm);
                throw std::runtime_error("inflate failed");
            }
        } while (ret != Z_STREAM_END);

        const size_t out_size = strm.total_out;
        inflateEnd(&strm);
        out.resize(out_size);
        return out;
    }
}

std::filesystem::path get_magic_file_path() {
#ifdef __APPLE__
    const char* home = getenv("HOME");
    return std::filesystem::path(home ? home : ".") /
           "Library/Application Support/monolith/magic.mgc";
#else
    const char* home = getenv("HOME");
    return std::filesystem::path(home ? home : ".") /
           ".local/share/monolith/magic.mgc";
#endif
}

void ensure_magic_installed() {
    const auto target = get_magic_file_path();
    if (!std::filesystem::exists(target)) {
        Logger::log(LogLevel::INFO, "Installing embedded magic.mgc to " + target.string(), "libmagic");
        std::filesystem::create_directories(target.parent_path());

        const auto decompressed = decompress_gzip(embedded_magic_mgc, embedded_magic_mgc_len);
        std::ofstream ofs(target, std::ios::binary);
        ofs.write(reinterpret_cast<const char*>(decompressed.data()), decompressed.size());
        ofs.close();
    }

#ifndef _WIN32
    setenv("MAGIC", target.c_str(), 1);
#else
    _putenv_s("MAGIC", target.string().c_str());
#endif
}

std::string detect_mime_type(const std::string &filename) {
    const magic_t magic = magic_open(MAGIC_MIME_TYPE | MAGIC_ERROR);
    if (!magic) {
        Logger::log(LogLevel::ERROR, "libmagic error: can't initialize libmagic", "libmagic");
        return {};
    }

    if (magic_load(magic, nullptr) != 0) {
        Logger::log(LogLevel::ERROR, "libmagic error: " + std::string(magic_error(magic)), "libmagic");
        magic_close(magic);
        return {};
    }

    const char *mime = magic_file(magic, filename.c_str());
    std::string result;
    if (mime) {
        result = mime;
        // sanitize: cut at newline and trim
        const auto pos = result.find_first_of("\r\n");
        if (pos != std::string::npos) result = result.substr(0, pos);
        while (!result.empty() && std::isspace(static_cast<unsigned char>(result.back()))) result.pop_back();
    } else {
        Logger::log(LogLevel::ERROR, "libmagic error: " + std::string(magic_error(magic)), "libmagic");
    }

    magic_close(magic);
    return result;
}

bool is_mpeg1_layer3_libmagic(const std::filesystem::path& path) {
    const magic_t magic = magic_open(MAGIC_MIME_TYPE | MAGIC_ERROR);
    if (!magic) return false;

    if (magic_load(magic, nullptr) != 0) {
        magic_close(magic);
        return false;
    }

    const char* desc = magic_file(magic, path.string().c_str());
    bool ok = false;
    if (desc) {
        const std::string s(desc);
        if (s.find("MPEG") != std::string::npos &&
            s.find("layer III") != std::string::npos &&
            (s.find("v1") != std::string::npos || s.find("version 1") != std::string::npos)) {
            ok = true;
        }
    }

    magic_close(magic);
    return ok;
}
