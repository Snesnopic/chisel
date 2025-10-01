//
// Created by Giuseppe Francione on 19/09/25.
//

#ifndef MONOLITH_ARCHIVE_FORMATS_HPP
#define MONOLITH_ARCHIVE_FORMATS_HPP

#include <unordered_map>
#include <string>
#include <optional>
#include <algorithm>

enum class ContainerFormat {
    Zip,
    SevenZip,
    Tar,
    GZip,
    BZip2,
    Xz,
    Rar,
    Wim,
    Mkv,
    Unknown
};

// MIME -> format
inline const std::unordered_map<std::string, ContainerFormat> mime_to_format = {
    { "application/zip",              ContainerFormat::Zip },
    { "application/x-zip-compressed", ContainerFormat::Zip },
    { "application/x-7z-compressed",  ContainerFormat::SevenZip },
    { "application/x-tar",            ContainerFormat::Tar },
    { "application/gzip",             ContainerFormat::GZip },
    { "application/x-bzip2",          ContainerFormat::BZip2 },
    { "application/x-xz",             ContainerFormat::Xz },
    { "application/vnd.rar",          ContainerFormat::Rar },
    { "application/x-rar-compressed", ContainerFormat::Rar },
    { "video/x-matroska",             ContainerFormat::Mkv },
    { "video/webm",                   ContainerFormat::Mkv },
    { "application/x-ms-wim",         ContainerFormat::Wim }
};

// format -> extension
inline std::string container_format_to_string(const ContainerFormat fmt) {
    switch (fmt) {
        case ContainerFormat::Zip:      return "zip";
        case ContainerFormat::SevenZip: return "7z";
        case ContainerFormat::Tar:      return "tar";
        case ContainerFormat::GZip:     return "gz";
        case ContainerFormat::BZip2:    return "bz2";
        case ContainerFormat::Xz:       return "xz";
        case ContainerFormat::Wim:      return "wim";
        case ContainerFormat::Mkv:      return "mkv";
        case ContainerFormat::Rar:      return "rar";
        default:                      return "unknown";
    }
}

inline std::optional<ContainerFormat> parse_container_format(const std::string &str) {
    std::string s = str;
    std::ranges::transform(s, s.begin(),
        [](unsigned char c){ return static_cast<char>(std::tolower(c)); });

    if (s == "zip")  return ContainerFormat::Zip;
    if (s == "7z")   return ContainerFormat::SevenZip;
    if (s == "tar")  return ContainerFormat::Tar;
    if (s == "gz" || s == "gzip")   return ContainerFormat::GZip;
    if (s == "bz2" || s == "bzip2") return ContainerFormat::BZip2;
    if (s == "xz")   return ContainerFormat::Xz;
    if (s == "wim")  return ContainerFormat::Wim;
    if (s == "rar")  return ContainerFormat::Rar;
    if (s == "mkv")  return ContainerFormat::Mkv;


    return std::nullopt;
}

// libarchive
inline bool can_read_format(const ContainerFormat fmt) {
    return fmt != ContainerFormat::Unknown;
}

inline bool can_write_format(const ContainerFormat fmt) {
    // libarchive doesn't write on RAR, WIM, 7z is limited
    switch (fmt) {
        case ContainerFormat::Zip:
        case ContainerFormat::Tar:
        case ContainerFormat::GZip:
        case ContainerFormat::BZip2:
        case ContainerFormat::Xz:
        case ContainerFormat::Mkv:
            return true;
        default:
            return false;
    }
}

#endif // MONOLITH_ARCHIVE_FORMATS_HPP