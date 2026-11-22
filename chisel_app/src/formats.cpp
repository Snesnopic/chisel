#include "chisel_app/formats.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace chisel_app {

    FileKind file_kind(std::string extension) {
        // ChiselSwift's FileCategory.category(for:), list by list
        static const std::unordered_set<std::string> image = {
            "jpg", "jpeg", "jpe", "jif", "jfif", "jfi", "thm", "png", "apng", "gif", "webp", "jxl", "tif", "tiff", "tga",
            "targa", "icb", "vda", "vst", "bmp", "dib", "ico", "cur", "icns", "gft", "pnm", "ppm", "pgm", "pbm", "jp2",
            "j2k", "j2c", "mng", "jng", "pcx", "dcx", "pcc", "ora", "svg", "psd", "psb"};
        static const std::unordered_set<std::string> audio = {
            "mp3", "m4a", "m4b", "wav", "flac", "ogg", "oga", "opus", "aif", "aiff", "aifc", "ape", "wv", "wvc", "wvp",
            "dsf", "dff", "mpc", "mp+", "tta"};
        static const std::unordered_set<std::string> video = {
            "mp4", "m4v", "mov", "qt", "3gp", "3g2", "mkv", "mka", "webm", "wma", "wmv", "asf", "swf", "avi", "ogv", "ogx"};
        static const std::unordered_set<std::string> document = {
            "pdf", "xml", "xhtml", "kml", "gpx", "dae", "rss", "atom", "xmp", "xsl", "xslt", "json", "docx", "docm",
            "dotm", "dotx", "xlsx", "xlsm", "xltm", "xltx", "pptx", "pptm", "potm", "potx", "ppsm", "ppsx", "doc", "xls",
            "ppt", "msi", "msp", "mst", "pub", "vsd", "vss", "adp", "mdb", "mdt", "mpd", "mpp", "mpt", "rvt", "sldasm",
            "slddrw", "sldprt", "snt", "spl", "dot", "xlt", "pps", "chm", "fla", "one", "ost", "rfa", "rte", "wps", "odt",
            "ods", "odp", "odg", "odf", "odb", "epub", "fb2", "cbz", "cbt", "vcf", "xps", "oxps", "exe", "dll", "ocx",
            "scr", "cpl", "sys", "drv", "bpl", "icl", "rll", "vbx", "dwfx", "3mf", "kmz", "xlam", "lua", "luac", "stl",
            "eml", "emlx", "msg", "mht", "mhtml", "mbx", "html", "htm", "css", "md", "markdown", "vcard", "xlsb", "ppam",
            "sldm", "thmx", "vsdx", "vsdm", "vssx", "vssm", "vstx", "vstm"};
        static const std::unordered_set<std::string> archive = {
            "zip", "air", "bsz", "cdr", "csl", "gallery", "gallerycollection", "galleryitem", "grs", "ita", "itz", "nbk",
            "notebook", "oex", "osk", "pk3", "puz", "stz", "vlt", "wal", "wba", "wmz", "wsz", "xap", "xl", "xmz", "xsn",
            "appx", "bar", "dwf", "easm", "rmskin", "sldx", "zipx", "tar", "gz", "tgz", "deb", "ipk", "svgz", "bz2", "xz",
            "lzma", "iso", "cpio", "a", "ar", "lib", "br", "zst", "tzst", "jar", "xpi", "apk", "ipa", "ipsw", "vsix",
            "nupkg", "war", "ear", "aab", "rdb", "knz", "cab", "lz4", "emz", "7z", "cb7"};
        static const std::unordered_set<std::string> font = {"woff", "woff2"};
        static const std::unordered_set<std::string> scientific = {"mseed", "mseed2", "mseed3"};
        static const std::unordered_set<std::string> database = {"sqlite", "sqlite3", "db", "thumbs.db"};
        if (!extension.empty() && extension.front() == '.') extension.erase(extension.begin());
        std::ranges::transform(extension, extension.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (image.contains(extension)) return FileKind::Image;
        if (audio.contains(extension)) return FileKind::Audio;
        if (video.contains(extension)) return FileKind::Video;
        if (document.contains(extension)) return FileKind::Document;
        if (archive.contains(extension)) return FileKind::Archive;
        if (font.contains(extension)) return FileKind::Font;
        if (scientific.contains(extension)) return FileKind::Scientific;
        if (database.contains(extension)) return FileKind::Database;
        return FileKind::Unknown;
    }

    std::string_view kind_name(const FileKind kind) {
        switch (kind) {
            case FileKind::Archive: return "Archive";
            case FileKind::Audio: return "Audio";
            case FileKind::Database: return "Database";
            case FileKind::Document: return "Document";
            case FileKind::Font: return "Font";
            case FileKind::Image: return "Image";
            case FileKind::Scientific: return "Scientific";
            case FileKind::Video: return "Video";
            case FileKind::Unknown: break;
        }
        return "Unknown";
    }

    std::map<FileKind, std::vector<std::string>> group_by_kind(const std::vector<std::string>& extensions) {
        std::map<FileKind, std::vector<std::string>> groups;
        for (const auto& ext : extensions) groups[file_kind(ext)].push_back(ext);
        for (auto& [kind, list] : groups) std::ranges::sort(list);
        return groups;
    }

} // namespace chisel_app
