//
// Created by Giuseppe Francione on 18/09/25.
//

#ifndef MONOLITH_FILE_TYPE_HPP
#define MONOLITH_FILE_TYPE_HPP

#include <string>
#include <unordered_map>
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
    Pdf,
    Docx,
    Xlsx,
    Pptx,
    Ods,
    Odt,
    Odp,
    Odg,
    Odf,
    Epub,
    Cbz,
    Cbt,
    Jar,
    Xpi,
    Ora,
    Dwfx,
    Xps,
    Apk,
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
    { "application/vnd.ms-powerpoint", ContainerFormat::Pptx},
    { "application/vnd.openxmlformats-officedocument.presentationml.presentation", ContainerFormat::Pptx },
    { "application/vnd.oasis.opendocument.presentation", ContainerFormat::Odp },
    { "application/vnd.oasis.opendocument.spreadsheet", ContainerFormat::Ods},
    { "application/vnd.oasis.opendocument.text", ContainerFormat::Odt},
    { "application/vnd.oasis.opendocument.graphics", ContainerFormat::Odg },
    { "application/vnd.oasis.opendocument.formula",  ContainerFormat::Odf },
    { "application/pdf",                ContainerFormat::Pdf},
    { "application/x-ms-wim",           ContainerFormat::Wim },
    { "application/epub+zip",           ContainerFormat::Epub },
    { "application/vnd.comicbook+zip",  ContainerFormat::Cbz },
    { "application/vnd.comicbook+tar",  ContainerFormat::Cbt },
    { "application/java-archive",       ContainerFormat::Jar },
    { "application/x-xpinstall",        ContainerFormat::Xpi },
    { "image/openraster",               ContainerFormat::Ora },
    { "model/vnd.dwfx+xps",             ContainerFormat::Dwfx },
    { "application/vnd.ms-xpsdocument", ContainerFormat::Xps },
    { "application/oxps",               ContainerFormat::Xps },
    { "application/vnd.android.package-archive", ContainerFormat::Apk },
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
        case ContainerFormat::Pdf:    return "pdf";
        //case ContainerFormat::Mkv:      return "mkv";
        case ContainerFormat::Rar:      return "rar";
        case ContainerFormat::Docx:     return "docx";
        case ContainerFormat::Xlsx:     return "xlsx";
        case ContainerFormat::Pptx:     return "pptx";
        case ContainerFormat::Ods:      return "ods";
        case ContainerFormat::Odt:      return "odt";
        case ContainerFormat::Odp:      return "odp";
        case ContainerFormat::Odg:      return "odg";
        case ContainerFormat::Odf:      return "odf";
        case ContainerFormat::Epub:     return "epub";
        case ContainerFormat::Cbz:      return "cbz";
        case ContainerFormat::Cbt:      return "cbt";
        case ContainerFormat::Jar:      return "jar";
        case ContainerFormat::Xpi:      return "xpi";
        case ContainerFormat::Ora:      return "ora";
        case ContainerFormat::Dwfx:     return "dwfx";
        case ContainerFormat::Xps:      return "xps";
        case ContainerFormat::Apk:      return "apk";
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
    if (s == "odg")   return ContainerFormat::Odg;
    if (s == "odf")   return ContainerFormat::Odf;
    if (s == "epub")  return ContainerFormat::Epub;
    if (s == "cbt")   return ContainerFormat::Cbt;
    if (s == "cbz")   return ContainerFormat::Cbz;
    if (s == "jar")   return ContainerFormat::Jar;
    if (s == "xpi")   return ContainerFormat::Xpi;
    if (s == "ora")   return ContainerFormat::Ora;
    if (s == "dwfx")  return ContainerFormat::Dwfx;
    if (s == "pdf")   return ContainerFormat::Pdf;
    if (s == "xps" || s == "oxps") return ContainerFormat::Xps;
    if (s == "apk") return ContainerFormat::Apk;
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
        case ContainerFormat::Odg:
        case ContainerFormat::Odf:
        case ContainerFormat::Epub:
        case ContainerFormat::Cbz:
        case ContainerFormat::Cbt:
        case ContainerFormat::Jar:
        case ContainerFormat::Xpi:
        case ContainerFormat::Ora:
        case ContainerFormat::Dwfx:
        case ContainerFormat::Xps:
        case ContainerFormat::Apk:
        case ContainerFormat::Pdf:
            return true;
        default:
            return false;
    }
}


static const std::unordered_map<std::string, std::string> ext_to_mime = {
    // archives
    {".zip",    "application/zip"},
    {".7z",     "application/x-7z-compressed"},
    {".cb7",    "application/x-7z-compressed"},
    {".tar",    "application/x-tar"},
    {".gz",     "application/gzip"},
    {".bz2",    "application/x-bzip2"},
    {".xz",     "application/x-xz"},
    {".wim",    "application/x-ms-wim"},
    {".rar",    "application/vnd.rar"},
    {".cbr",    "application/vnd.comicbook+rar"},
    {".iso",    "application/x-iso9660-image"},
    {".cpio",   "application/x-cpio"},
    {".lzma",   "application/x-lzma"},
    {".cab",    "application/vnd.ms-cab-compressed"},
    {".epub",   "application/epub+zip"},
    {".cbz",    "application/vnd.comicbook+zip"},
    {".cbt",    "application/vnd.comicbook+tar"},
    {".jar",    "application/java-archive"},
    {".xpi",    "application/x-xpinstall"},
    {".ora",    "image/openraster"},
    {".dwfx",   "model/vnd.dwfx+xps"},
    {".xps",    "application/vnd.ms-xpsdocument"},
    {".oxps",   "application/oxps"},
    {".apk",    "application/vnd.android.package-archive"},

    // images
    {".jpg",    "image/jpeg"},
    {".jpeg",   "image/jpeg"},
    {".png",    "image/png"},
    {".jxl",    "image/jxl"},
    {".tif",    "image/tiff"},
    {".tiff",   "image/tiff"},
    {".webp",   "image/webp"},
    {".gif",    "image/gif"},
    {".svg",    "image/svg+xml"},

    // documents (office open xml)
    {".docx",   "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
    {".xlsx",   "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
    {".pptx",   "application/vnd.openxmlformats-officedocument.presentationml.presentation"},

    // documents (legacy office)
    {".doc",    "application/msword"},
    {".xls",    "application/vnd.ms-excel"},
    {".ppt",    "application/vnd.ms-powerpoint"},

    // documents (open document format)
    {".odt",    "application/vnd.oasis.opendocument.text"},
    {".ods",    "application/vnd.oasis.opendocument.spreadsheet"},
    {".odp",    "application/vnd.oasis.opendocument.presentation"},
    {".odg",    "application/vnd.oasis.opendocument.graphics"},
    {".odf",    "application/vnd.oasis.opendocument.formula"},
    {".pdf",    "application/pdf"},

    // databases
    {".sqlite", "application/vnd.sqlite3"},
    {".db",     "application/vnd.sqlite3"},

    // audio
    {".flac",   "audio/flac"},
    {".wv",     "audio/x-wavpack"},
    {".wvp",    "audio/x-wavpack"},
    {".wvc",    "audio/x-wavpack"},
    {".mp3",    "audio/mpeg"},
    {".wav",    "audio/wav"},
    {".ape",    "audio/x-ape"},

    // video / containers
    {".mkv",    "video/x-matroska"},
    {".webm",   "video/webm"},

    // scientific / seismic
    {".mseed",  "application/vnd.fdsn.mseed"}
};



#endif //MONOLITH_FILE_TYPE_HPP