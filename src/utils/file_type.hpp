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
    {".zip",    "application/zip"},
    {".7z",     "application/x-7z-compressed"},
    {".tar",    "application/x-tar"},
    {".gz",     "application/gzip"},
    {".bz2",    "application/x-bzip2"},
    {".xz",     "application/x-xz"},
    {".wim",    "application/x-ms-wim"},
    {".rar",    "application/vnd.rar"},
    {".iso",    "application/x-iso9660-image"},
    {".cpio",   "application/x-cpio"},
    {".lzma",   "application/x-lzma"},
    {".cab",    "application/vnd.ms-cab-compressed"},
    {".epub",   "application/epub+zip"},
    {".cbz",    "application/vnd.comicbook+zip"},
    {".cbt",    "application/vnd.comicbook+tar"},

    // images
    {".jpg",    "image/jpeg"},
    {".jpeg",   "image/jpeg"},
    {".png",    "image/png"},
    {".jxl",    "image/jxl"},
    {".tif",    "image/tiff"},
    {".tiff",   "image/tiff"},
    {".webp",   "image/webp"},

    // documents
    {".pdf",    "application/pdf"},
    {".docx",   "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
    {".xlsx",   "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
    {".pptx",   "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
    {".odt",    "application/vnd.oasis.opendocument.text"},
    {".ods",    "application/vnd.oasis.opendocument.spreadsheet"},
    {".odp",    "application/vnd.oasis.opendocument.presentation"},
    {".sqlite", "application/vnd.sqlite3"},

    // audio
    {".flac",   "audio/flac"},
    {".wv",     "audio/x-wavpack"},
    {".wvp",    "audio/x-wavpack"},
    {".wvc",    "audio/x-wavpack"},
    {".mp3",    "audio/mpeg"},
    {".wav",    "audio/wav"},

    // video / containers
    {".mkv",    "video/x-matroska"},
    {".webm",   "video/webm"},

    // scientific / seismic
    {".mseed",  "application/vnd.fdsn.mseed"}
};

std::string detect_mime_type(const std::string &filename);
bool is_mpeg1_layer3_libmagic(const std::filesystem::path& path);


#endif //MONOLITH_FILE_TYPE_HPP
