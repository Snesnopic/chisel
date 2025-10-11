//
// Created by Giuseppe Francione on 11/10/25.
//
#ifndef WIN32
#include <magic.h>
#endif
#include "mime_detector.hpp"
#include "file_type.hpp"
#include "logger.hpp"

#include "mime_detector.hpp"
#include "file_type.hpp"
#include "logger.hpp"

#ifndef _WIN32
#include <magic.h>
#endif

std::string MimeDetector::detect(const std::filesystem::path& path) {
#ifndef _WIN32
    const magic_t magic = magic_open(MAGIC_MIME_TYPE | MAGIC_ERROR);
    if (!magic) return {};
    if (magic_load(magic, nullptr) != 0) {
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

bool MimeDetector::is_mpeg1_layer3(const std::filesystem::path& path) {
#ifndef _WIN32
    const magic_t magic = magic_open(MAGIC_MIME_TYPE | MAGIC_ERROR);
    if (!magic) return false;
    if (magic_load(magic, nullptr) != 0) {
        magic_close(magic);
        return false;
    }
    const char* desc = magic_file(magic, path.string().c_str());
    bool ok = false;
    if (desc) {
        std::string s(desc);
        if (s.find("MPEG") != std::string::npos &&
            s.find("layer III") != std::string::npos &&
            (s.find("v1") != std::string::npos || s.find("version 1") != std::string::npos)) {
            ok = true;
            }
    }
    magic_close(magic);
    return ok;
#else
    // fallback: controlla solo estensione .mp3
    return path.extension() == ".mp3";
#endif
}