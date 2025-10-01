//
// Created by Giuseppe Francione on 18/09/25.
//

#include "file_type.hpp"
#include <filesystem>
#include <magic.h>
#include "logger.hpp"

std::string detect_mime_type(const std::string &filename) {
    const magic_t magic = magic_open(MAGIC_MIME_TYPE);
    if (!magic) {
        Logger::log(LogLevel::ERROR, "libmagic error: can't initialize libmagic ", "libmagic");
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
    } else {
        Logger::log(LogLevel::ERROR, "libmagic error: " + std::string(magic_error(magic)), "libmagic");
    }

    magic_close(magic);
    return result;
}

bool is_mpeg1_layer3_libmagic(const std::filesystem::path& path) {
    const magic_t magic = magic_open(MAGIC_MIME_TYPE | MAGIC_CONTINUE | MAGIC_RAW);
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
