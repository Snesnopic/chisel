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
    //Mkv,
    Docx,
    Xlsx,
    Pptx,
    Ods,
    Odt,
    Odp,
    Epub,
    Cbz,
    Cbt,
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
    //{ "video/x-matroska",             ContainerFormat::Mkv },
    //{ "video/webm",                   ContainerFormat::Mkv },
    { "application/vnd.openxmlformats-officedocument.wordprocessingml.document", ContainerFormat::Docx },
    { "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",       ContainerFormat::Xlsx },
    { "application/vnd.openxmlformats-officedocument.presentationml.presentation", ContainerFormat::Pptx },
    { "application/vnd.oasis.opendocument.presentation", ContainerFormat::Odp },
    { "application/vnd.oasis.opendocument.spreadsheet", ContainerFormat::Ods},
    { "application/vnd.oasis.opendocument.text", ContainerFormat::Odt},
    { "application/x-ms-wim",           ContainerFormat::Wim },
    { "application/epub+zip",           ContainerFormat::Epub },
    { "application/vnd.comicbook+zip",  ContainerFormat::Cbz },
    { "application/vnd.comicbook+tar",  ContainerFormat::Cbt }
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
        //case ContainerFormat::Mkv:      return "mkv";
        case ContainerFormat::Rar:      return "rar";
        case ContainerFormat::Docx:     return "docx";
        case ContainerFormat::Xlsx:     return "xlsx";
        case ContainerFormat::Pptx:     return "pptx";
        case ContainerFormat::Ods:      return "ods";
        case ContainerFormat::Odt:      return "odt";
        case ContainerFormat::Odp:      return "odp";
        case ContainerFormat::Epub:    return "epub";
        case ContainerFormat::Cbz:     return "cbz";
        case ContainerFormat::Cbt:    return "cbt";
        default:                        return "unknown";
    }
}

// extension -> format
inline std::optional<ContainerFormat> parse_container_format(const std::string &str) {
    std::string s = str;
    std::ranges::transform(s, s.begin(),
        [](unsigned char c){ return static_cast<char>(std::tolower(c)); });

    if (s == "zip")   return ContainerFormat::Zip;
    if (s == "7z")    return ContainerFormat::SevenZip;
    if (s == "tar")   return ContainerFormat::Tar;
    if (s == "gz" || s == "gzip")    return ContainerFormat::GZip;
    if (s == "bz2" || s == "bzip2")  return ContainerFormat::BZip2;
    if (s == "xz")    return ContainerFormat::Xz;
    if (s == "wim")   return ContainerFormat::Wim;
    if (s == "rar")   return ContainerFormat::Rar;
    //if (s == "mkv")   return ContainerFormat::Mkv;
    if (s == "docx")  return ContainerFormat::Docx;
    if (s == "xlsx")  return ContainerFormat::Xlsx;
    if (s == "pptx")  return ContainerFormat::Pptx;
    if (s == "ods")   return ContainerFormat::Ods;
    if (s == "odt")   return ContainerFormat::Odt;
    if (s == "odp")   return ContainerFormat::Odp;
    if (s == "epub")  return ContainerFormat::Epub;
    if (s == "cbt")   return ContainerFormat::Cbt;
    if (s == "cbz")   return ContainerFormat::Cbz;
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
        //case ContainerFormat::Mkv:
        case ContainerFormat::Docx:
        case ContainerFormat::Xlsx:
        case ContainerFormat::Pptx:
        case ContainerFormat::Ods:
        case ContainerFormat::Odt:
        case ContainerFormat::Odp:
        case ContainerFormat::Epub:
        case ContainerFormat::Cbz:
        case ContainerFormat::Cbt:
            return true;
        default:
            return false;
    }
}

#endif // MONOLITH_ARCHIVE_FORMATS_HPP