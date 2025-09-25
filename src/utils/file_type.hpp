//
// Created by Giuseppe Francione on 18/09/25.
//

#ifndef MONOLITH_FILE_TYPE_HPP
#define MONOLITH_FILE_TYPE_HPP

#include <filesystem>
#include <string>
#include <unordered_map>

static const std::unordered_map<std::string, std::string> ext_to_mime = {
    // archives
    {".zip",  "application/zip"},
    {".7z",   "application/x-7z-compressed"},
    {".tar",  "application/x-tar"},
    {".gz",   "application/gzip"},
    {".bz2",  "application/x-bzip2"},
    {".xz",   "application/x-xz"},
    {".wim",  "application/x-ms-wim"},
    {".rar",  "application/vnd.rar"},
    // images
    {".jpg",  "image/jpeg"},
    {".jpeg", "image/jpeg"},
    {".png",  "image/png"},
    // audio
    {".flac", "audio/flac"}
};


std::string detect_mime_type(const std::string &filename);
bool is_mpeg1_layer3_libmagic(const std::filesystem::path& path);


#endif //MONOLITH_FILE_TYPE_HPP
