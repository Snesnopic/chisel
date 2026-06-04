//
// Created by Giuseppe Francione on 18/09/25.
//

/**
 * @file file_type.hpp
 * @brief Defines the central enumeration for container formats and utility functions.
 *
 * This file provides the core ContainerFormat enum, which classifies all
 * archive-like formats chisel can interact with. It also provides maps
 * and functions to convert between enums, MIME types, extensions, and
 * string representations.
 */

#ifndef CHISEL_FILE_TYPE_HPP
#define CHISEL_FILE_TYPE_HPP

#include <string>
#include <unordered_map>
#include <optional>
#include <algorithm>

namespace chisel {

/**
 * @brief Enumerates all known container types chisel can process.
 *
 * This is used to identify formats that can be extracted (e.g., Zip, Odf)
 * or formats that are containers but treated as single files (e.g., Pdf).
 */
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
    Iso,
    Cpio,
    Ar,
    Zstd,
    Kanzi,
    Vcf,
    Pe,
    Cfbf,
    Json,
    Xml,
    Unknown
};

///< Map linking MIME type strings to their corresponding ContainerFormat.
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
    { "application/x-iso9660-image", ContainerFormat::Iso },
    { "application/x-cpio",          ContainerFormat::Cpio },
    { "application/x-archive",       ContainerFormat::Ar },
    { "application/zstd",            ContainerFormat::Zstd },
    { "application/x-zstd",          ContainerFormat::Zstd },
    { "application/vnd.comicbook+rar",  ContainerFormat::Rar },
    { "application/x-cbr",              ContainerFormat::Rar },
    { "application/vnd.ms-package.3dmanufacturing-3dmodel+xml", ContainerFormat::Zip },
    { "application/vnd.google-earth.kmz",       ContainerFormat::Zip },
    { "application/vsix",                       ContainerFormat::Zip },
    { "application/zip",                        ContainerFormat::Zip },
    { "application/java-archive",               ContainerFormat::Zip },
    { "application/vnd.android.package-archive", ContainerFormat::Apk },
    { "application/x-kanzi",                    ContainerFormat::Kanzi },
    { "text/vcard",                             ContainerFormat::Vcf },
    { "text/x-vcard",                           ContainerFormat::Vcf },
    { "application/x-msdownload",               ContainerFormat::Pe },
    { "application/vnd.microsoft.portable-executable", ContainerFormat::Pe }
};

/**
 * @brief Converts a ContainerFormat enum to its lowercase string representation.
 * @param fmt The ContainerFormat enum value.
 * @return A string (e.g., "zip", "pdf", "unknown").
 */
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
        case ContainerFormat::Mkv:      return "mkv";
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
        case ContainerFormat::Iso:      return "iso";
        case ContainerFormat::Cpio:     return "cpio";
        case ContainerFormat::Ar:       return "a";
        case ContainerFormat::Zstd:     return "zst";
        case ContainerFormat::Kanzi:    return "knz";
        case ContainerFormat::Vcf:      return "vcf";
        case ContainerFormat::Pe:       return "pe";
        case ContainerFormat::Cfbf:     return "cfbf";
        case ContainerFormat::Json:     return "json";
        case ContainerFormat::Xml:      return "xml";
        default:                        return "unknown";
    }
}

/**
 * @brief Parses a string (typically a file extension) into a ContainerFormat.
 *
 * The input string is case-insensitive (e.g., "zip", "Zip", "ZIP" all work).
 *
 * @param str The string to parse.
 * @return std::optional<ContainerFormat> containing the enum if successful,
 * std::nullopt otherwise.
 */
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
    if (s == "mkv")   return ContainerFormat::Mkv;
    if (s == "docx" || s == "docm" || s == "dotm" || s == "dotx")  return ContainerFormat::Docx;
    if (s == "xlsx" || s == "xlsm" || s == "xltm" || s == "xltx" || s == "xl")  return ContainerFormat::Xlsx;
    if (s == "pptx" || s == "pptm" || s == "potm" || s == "potx" || s == "ppsm" || s == "ppsx")  return ContainerFormat::Pptx;
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
    if (s == "apk" || s == "ipa" || s == "ipsw") return ContainerFormat::Apk;
    if (s == "iso")  return ContainerFormat::Iso;
    if (s == "cpio") return ContainerFormat::Cpio;
    if (s == "a" || s == "ar" || s == "lib") return ContainerFormat::Ar;
    if (s == "zst" || s == "zstd" || s == "tzst") return ContainerFormat::Zstd;
    if (s == "3mf" || s == "kmz" || s == "vsix" || s == "nupkg" || s == "air" || s == "bsz" || s == "cdr" || s == "csl" || s == "grs" || s == "ita" || s == "itz" || s == "nbk" || s == "notebook" || s == "oex" || s == "osk" || s == "pk3" || s == "puz" || s == "stz" || s == "vlt" || s == "wal" || s == "wba" || s == "wmz" || s == "wsz" || s == "xap" || s == "xmz" || s == "xsn" || s == "gallery" || s == "gallerycollection" || s == "galleryitem" || s == "appx" || s == "bar" || s == "dwf" || s == "easm" || s == "rmskin" || s == "sldx" || s == "zipx") return ContainerFormat::Zip;
    if (s == "war" || s == "ear") return ContainerFormat::Jar;
    if (s == "aab")   return ContainerFormat::Apk;
    if (s == "knz") return ContainerFormat::Kanzi;
    if (s == "vcf" || s == "vcard") return ContainerFormat::Vcf;
    if (s == "json") return ContainerFormat::Json;
    if (s == "xml" || s == "fb2" || s == "fxg" || s == "kml" || s == "xsl" || s == "xslt" || s == "xhtml") return ContainerFormat::Xml;
    if (s == "exe" || s == "dll" || s == "ocx" || s == "scr" || s == "cpl") return ContainerFormat::Pe;
    if (s == "doc" || s == "xls" || s == "ppt" || s == "msi" || s == "msp" || s == "mst" || s == "pub" || s == "vsd" || s == "vss" || s == "vst" || s == "adp" || s == "mdb" || s == "mdt" || s == "mpd" || s == "mpp" || s == "mpt" || s == "rvt" || s == "sldasm" || s == "slddrw" || s == "sldprt" || s == "snt" || s == "thumbs.db" || s == "chm" || s == "fla" || s == "one" || s == "ost" || s == "rfa" || s == "rte" || s == "wps") return ContainerFormat::Cfbf;
    if (s == "gz" || s == "tgz" || s == "deb" || s == "ipk" || s == "svgz") return ContainerFormat::GZip;

    return std::nullopt;
}

/**
 * @brief Checks if a format is readable by the archive processor (libarchive).
 * @param fmt The ContainerFormat to check.
 * @return true if the format is supported for reading.
 */
inline bool can_read_format(const ContainerFormat fmt) {
    return fmt != ContainerFormat::Unknown;
}

/**
 * @brief Checks if a format is writable by the archive processor (libarchive).
 *
 * This is used to determine if an archive can be *re-created*.
 * Formats like RAR can be read but not written.
 *
 * @param fmt The ContainerFormat to check.
 * @return true if libarchive supports writing this format.
 */
inline bool can_write_format(const ContainerFormat fmt) {
    // libarchive doesn't write on RAR, WIM, 7z is limited
    switch (fmt) {
        case ContainerFormat::Zip:
        case ContainerFormat::Tar:
        case ContainerFormat::GZip:
        case ContainerFormat::BZip2:
        case ContainerFormat::Xz:
        case ContainerFormat::Mkv:
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
        case ContainerFormat::Iso:
        case ContainerFormat::Cpio:
        case ContainerFormat::Ar:
        case ContainerFormat::Zstd:
        case ContainerFormat::Kanzi:
        case ContainerFormat::Vcf:
        case ContainerFormat::Pe:
        case ContainerFormat::Cfbf:
        case ContainerFormat::Json:
        case ContainerFormat::Xml:
            return true;
        default:
            return false;
    }
}

///< Map linking common file extensions (lowercase) to their primary MIME type.
static const std::unordered_map<std::string, std::string> ext_to_mime = {
    // archives
    {".zip",    "application/zip"},
    {".air",    "application/zip"},
    {".appx",   "application/zip"},
    {".bar",    "application/zip"},
    {".bsz",    "application/zip"},
    {".cdr",    "application/zip"},
    {".csl",    "application/zip"},
    {".dwf",    "application/zip"},
    {".easm",   "application/zip"},
    {".gallery", "application/zip"},
    {".gallerycollection", "application/zip"},
    {".galleryitem", "application/zip"},
    {".grs",    "application/zip"},
    {".ipa",    "application/zip"},
    {".ipsw",   "application/zip"},
    {".ita",    "application/zip"},
    {".itz",    "application/zip"},
    {".nbk",    "application/zip"},
    {".notebook", "application/zip"},
    {".oex",    "application/zip"},
    {".osk",    "application/zip"},
    {".pk3",    "application/zip"},
    {".puz",    "application/zip"},
    {".rmskin", "application/zip"},
    {".sldx",   "application/zip"},
    {".stz",    "application/zip"},
    {".vlt",    "application/zip"},
    {".wal",    "application/zip"},
    {".wba",    "application/zip"},
    {".wmz",    "application/zip"},
    {".wsz",    "application/zip"},
    {".xap",    "application/zip"},
    {".xl",     "application/zip"},
    {".xmz",    "application/zip"},
    {".xsn",    "application/zip"},
    {".zipx",   "application/zip"},
    {".7z",     "application/x-7z-compressed"},
    {".cb7",    "application/x-7z-compressed"},
    {".tar",    "application/x-tar"},
    {".gz",     "application/gzip"},
    {".tgz",    "application/gzip"},
    {".deb",    "application/gzip"},
    {".ipk",    "application/gzip"},
    {".svgz",   "application/gzip"},
    {".bz2",    "application/x-bzip2"},
    {".xz",     "application/x-xz"},
    {".wim",    "application/x-ms-wim"},
    {".rar",    "application/vnd.rar"},
    {".cbr",    "application/vnd.comicbook+rar"},
    {".iso",    "application/x-iso9660-image"},
    {".cpio",   "application/x-cpio"},
    {".lzma",   "application/x-lzma"},
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
    {".cbr",    "application/vnd.comicbook+rar"},
    {".3mf",    "application/vnd.ms-package.3dmanufacturing-3dmodel+xml"},
    {".kmz",    "application/vnd.google-earth.kmz"},
    {".vsix",   "application/zip"},
    {".nupkg",  "application/zip"},
    {".war",    "application/java-archive"},
    {".ear",    "application/java-archive"},
    {".aab",    "application/vnd.android.package-archive"},
    {".knz",    "application/x-kanzi"},
    {".vcf",    "text/vcard"},
    {".json",   "application/json"},
    {".exe",    "application/x-msdownload"},
    {".dll",    "application/x-msdownload"},
    {".ocx",    "application/x-msdownload"},
    {".scr",    "application/x-msdownload"},
    {".cpl",    "application/x-msdownload"},

    // images
    {".jpg",    "image/jpeg"},
    {".jpeg",   "image/jpeg"},
    {".jpe",    "image/jpeg"},
    {".jif",    "image/jpeg"},
    {".jfif",   "image/jpeg"},
    {".jfi",    "image/jpeg"},
    {".thm",    "image/jpeg"},
    {".png",    "image/png"},
    {".apng",   "image/png"},
    {".jxl",    "image/jxl"},
    {".tif",    "image/tiff"},
    {".tiff",   "image/tiff"},
    {".webp",   "image/webp"},
    {".gif",    "image/gif"},
    {".svg",    "image/svg+xml"},

    // documents (office open xml)
    {".docx",   "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
    {".docm",   "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
    {".dotm",   "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
    {".dotx",   "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
    {".xlsx",   "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
    {".xlsm",   "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
    {".xltm",   "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
    {".xltx",   "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
    {".pptx",   "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
    {".pptm",   "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
    {".potm",   "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
    {".potx",   "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
    {".ppsm",   "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
    {".ppsx",   "application/vnd.openxmlformats-officedocument.presentationml.presentation"},

    // documents (legacy office / CFBF)
    {".doc",    "application/msword"},
    {".xls",    "application/vnd.ms-excel"},
    {".ppt",    "application/vnd.ms-powerpoint"},
    {".msi",    "application/x-msi"},
    {".msp",    "application/x-msi"},
    {".mst",    "application/x-msi"},
    {".pub",    "application/x-ole-storage"},
    {".vsd",    "application/x-ole-storage"},
    {".vss",    "application/x-ole-storage"},
    {".vst",    "application/x-ole-storage"},
    {".adp",    "application/x-ole-storage"},
    {".mdb",    "application/x-ole-storage"},
    {".mdt",    "application/x-ole-storage"},
    {".mpd",    "application/x-ole-storage"},
    {".mpp",    "application/x-ole-storage"},
    {".mpt",    "application/x-ole-storage"},
    {".rvt",    "application/x-ole-storage"},
    {".sldasm", "application/x-ole-storage"},
    {".slddrw", "application/x-ole-storage"},
    {".sldprt", "application/x-ole-storage"},
    {".snt",    "application/x-ole-storage"},
    {".thumbs.db", "application/x-ole-storage"},
    {".chm",    "application/x-ole-storage"},
    {".fla",    "application/x-ole-storage"},
    {".one",    "application/x-ole-storage"},
    {".ost",    "application/x-ole-storage"},
    {".rfa",    "application/x-ole-storage"},
    {".rte",    "application/x-ole-storage"},
    {".wps",    "application/x-ole-storage"},

    // documents (open document format)
    {".odt",    "application/vnd.oasis.opendocument.text"},
    {".ods",    "application/vnd.oasis.opendocument.spreadsheet"},
    {".odp",    "application/vnd.oasis.opendocument.presentation"},
    {".odg",    "application/vnd.oasis.opendocument.graphics"},
    {".odf",    "application/vnd.oasis.opendocument.formula"},
    {".odb",    "application/vnd.oasis.opendocument.database"},
    {".pdf",    "application/pdf"},

    // xml variants
    {".xml",    "application/xml"},
    {".xsl",    "application/xml"},
    {".xslt",   "application/xml"},

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

} // namespace chisel

#endif //CHISEL_FILE_TYPE_HPP