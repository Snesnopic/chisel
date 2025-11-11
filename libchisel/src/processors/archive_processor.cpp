//
// Created by Giuseppe Francione on 20/10/25.
//

#include "../../include/archive_processor.hpp"
#include "../../include/logger.hpp"
#include "../../include/mime_detector.hpp"
#include "../../include/file_type.hpp"
#include "../../include/random_utils.hpp"
#include <archive.h>
#include <archive_entry.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <optional>
#include <system_error>
#include <unordered_map>
#include <algorithm>
#include <chrono>
#include <cctype>
#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace chisel {

namespace fs = std::filesystem;

static const char* processor_tag() {
    return "ArchiveProcessor";
}

// --- helpers ---

static fs::path make_temp_dir() {
    const auto base = fs::temp_directory_path();
    const auto now  = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path dir = base / ("chisel_" + std::to_string(now) + "_" + RandomUtils::random_suffix());
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

static std::string to_lower_copy(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static std::string rel_path_of(const fs::path& root, const fs::path& p) {
    std::error_code ec;
    const auto rel = fs::relative(p, root, ec);
    std::string s = rel.generic_string();
    return s.empty() ? p.filename().generic_string() : s;
}

static bool ensure_parent_dirs(const fs::path& p, std::error_code& ec) {
    const auto parent = p.parent_path();
    if (parent.empty()) return true;
    if (fs::exists(parent, ec)) return !ec;
    fs::create_directories(parent, ec);
    return !ec;
}

// natural comparator for filenames (numeric-aware)
static bool natural_less_string(const std::string& sa, const std::string& sb) {
    size_t i = 0, j = 0;
    while (i < sa.size() && j < sb.size()) {
        if (std::isdigit(static_cast<unsigned char>(sa[i])) && std::isdigit(static_cast<unsigned char>(sb[j]))) {
            size_t ia = i, jb = j;
            while (ia < sa.size() && std::isdigit(static_cast<unsigned char>(sa[ia]))) ++ia;
            while (jb < sb.size() && std::isdigit(static_cast<unsigned char>(sb[jb]))) ++jb;
            std::string as = sa.substr(i, ia - i);
            std::string bs = sb.substr(j, jb - j);
            auto strip_leading = [](const std::string &s)->std::string {
                size_t k = 0;
                while (k + 1 < s.size() && s[k] == '0') ++k;
                return s.substr(k);
            };
            std::string as2 = strip_leading(as);
            std::string bs2 = strip_leading(bs);
            if (as2.size() != bs2.size()) return as2.size() < bs2.size();
            if (as2 != bs2) return as2 < bs2;
            i = ia; j = jb;
        } else {
            if (sa[i] != sb[j]) return sa[i] < sb[j];
            ++i; ++j;
        }
    }
    return sa.size() < sb.size();
}

static bool natural_less_path(const fs::path& a, const fs::path& b, const fs::path& root) {
    const std::string sa = rel_path_of(root, a);
    const std::string sb = rel_path_of(root, b);
    return natural_less_string(sa, sb);
}

// sanitize a candidate archive entry path to avoid zip-slip
static bool sanitize_archive_entry_path(const std::string& entry_name, const fs::path& dest_dir, fs::path& out_path) {
    if (entry_name.empty()) return false;
    if (entry_name.find('\0') != std::string::npos) return false;

    std::string s = entry_name;
    for (auto& c : s) { if (c == '\\') c = '/'; }
    while (!s.empty() && s.front() == '/') s.erase(s.begin());

    fs::path candidate = fs::path(dest_dir) / fs::path(s).relative_path();
    auto normalized = candidate.lexically_normal();
    auto base = fs::path(dest_dir).lexically_normal();

    const auto ns = normalized.string();
    const auto bs = base.string();
    if (ns.size() < bs.size()) return false;
    if (ns.compare(0, bs.size(), bs) != 0) return false;

    out_path = normalized;
    return true;
}

// --- format detection ---

static ContainerFormat detect_format(const fs::path& path) {
    const std::string mime = MimeDetector::detect(path);
    if (!mime.empty()) {
        auto it = mime_to_format.find(mime);
        if (it != mime_to_format.end()) return it->second;
    }

    std::string ext = to_lower_copy(path.extension().string());
    if (!ext.empty() && ext.front() == '.') ext.erase(0, 1);
    if (!ext.empty()) {
        if (auto parsed = parse_container_format(ext)) {
            return *parsed;
        }
        const auto fname = to_lower_copy(path.filename().string());
        if (fname.ends_with(".tar.gz"))  return ContainerFormat::Tar;
        if (fname.ends_with(".tar.bz2")) return ContainerFormat::Tar;
        if (fname.ends_with(".tar.xz"))  return ContainerFormat::Tar;
    }
    return ContainerFormat::Unknown;
}

static bool is_archive_file(const fs::path& path, ContainerFormat& fmt_out) {
    fmt_out = detect_format(path);
    return fmt_out != ContainerFormat::Unknown && can_read_format(fmt_out);
}

// --- libarchive extract/create ---

static bool extract_with_libarchive(const fs::path& archive_path, const fs::path& dest_dir) {
    struct archive* a = archive_read_new();
    struct archive_entry* entry = nullptr;

    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);
    archive_read_set_options(a, "hdrcharset=UTF-8");

    int r = archive_read_open_filename(a, archive_path.string().c_str(), 10240);
    if (r == ARCHIVE_WARN) {
        Logger::log(LogLevel::Warning, std::string("LIBARCHIVE WARN: ") + archive_error_string(a), processor_tag());
    }
    if (r != ARCHIVE_OK) {
        Logger::log(LogLevel::Error, "archive_read_open_filename: " + std::string(archive_error_string(a)), processor_tag());
        archive_read_free(a);
        return false;
    }

    std::error_code ec;
    std::vector<char> buffer(64 * 1024);

    while ((r = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
        const char* current = archive_entry_pathname(entry);
        if (!current) {
            archive_read_data_skip(a);
            continue;
        }

        fs::path out_path;
        if (!sanitize_archive_entry_path(current, dest_dir, out_path)) {
            Logger::log(LogLevel::Warning, "Skipping suspicious archive entry (path traversal): " + std::string(current), processor_tag());
            archive_read_data_skip(a);
            continue;
        }

        if (!ensure_parent_dirs(out_path, ec)) {
            Logger::log(LogLevel::Error, "Can't create folder for: " + out_path.string(), processor_tag());
            archive_read_data_skip(a);
            continue;
        }

        if (archive_entry_filetype(entry) == AE_IFDIR) {
            fs::create_directories(out_path, ec);
            archive_read_data_skip(a);
            continue;
        }

        if (archive_entry_filetype(entry) == AE_IFLNK) {
            const char* link_target = archive_entry_symlink(entry);
            if (link_target && link_target[0]) {
                std::error_code rc;
                fs::create_directories(out_path.parent_path(), rc);
#if defined(_WIN32)
                std::error_code tmp_ec;
                fs::create_symlink(fs::path(link_target), out_path, tmp_ec);
                (void)tmp_ec;
#else
                std::error_code sce;
                fs::create_symlink(fs::path(link_target), out_path, sce);
#endif
            }
            archive_read_data_skip(a);
            continue;
        }

        std::ofstream ofs(out_path, std::ios::binary);
        if (!ofs) {
            Logger::log(LogLevel::Error, "Can't open file in write mode: " + out_path.string(), processor_tag());
            archive_read_data_skip(a);
            continue;
        }

        la_ssize_t size_read = 0;
        while ((size_read = archive_read_data(a, buffer.data(), buffer.size())) > 0) {
            ofs.write(buffer.data(), static_cast<std::streamsize>(size_read));
        }
        ofs.close();

        if (size_read < 0) {
            Logger::log(LogLevel::Error, "Error reading data: " + std::string(archive_error_string(a)), processor_tag());
            archive_read_free(a);
            return false;
        }
    }

    if (r != ARCHIVE_EOF) {
        Logger::log(LogLevel::Error, "Error during iteration: " + std::string(archive_error_string(a)), processor_tag());
        archive_read_free(a);
        return false;
    }

    archive_read_free(a);
    return true;
}

struct PairHash {
    template <class T1, class T2>
    std::size_t operator()(const std::pair<T1,T2>& p) const noexcept {
        auto h1 = std::hash<T1>{}(p.first);
        auto h2 = std::hash<T2>{}(p.second);
        return h1 ^ (h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
    }
};

static bool create_with_libarchive(const fs::path& src_dir, const fs::path& out_path, ContainerFormat fmt) {
    archive* a = archive_write_new();
    if (!a) return false;

    int r = ARCHIVE_OK;

    switch (fmt) {
        case ContainerFormat::Epub:
            r = archive_write_set_format_zip(a);
            if (r == ARCHIVE_OK) {
                archive_write_set_format_option(a, "zip", "compression", "store");
            }
            break;
        case ContainerFormat::Zip:
        case ContainerFormat::Cbz:
        case ContainerFormat::Jar:
        case ContainerFormat::Xpi:
        case ContainerFormat::Ora:
        case ContainerFormat::Dwfx:
        case ContainerFormat::Xps:
        case ContainerFormat::Apk:
            r = archive_write_set_format_zip(a);
            if (r == ARCHIVE_OK) {
                archive_write_set_format_option(a, "zip", "compression", "deflate");
                archive_write_set_format_option(a, "zip", "compression-level", "9");
            }
            break;
        case ContainerFormat::Tar:
        case ContainerFormat::Cbt:
            r = archive_write_set_format_pax_restricted(a);
            break;
        case ContainerFormat::GZip:
            r = archive_write_set_format_pax_restricted(a);
            if (r == ARCHIVE_OK) {
                r = archive_write_add_filter_gzip(a);
                if (r == ARCHIVE_OK) {
                    archive_write_set_filter_option(a, "gzip", "compression-level", "9");
                }
            }
            break;
        case ContainerFormat::BZip2:
            r = archive_write_set_format_pax_restricted(a);
            if (r == ARCHIVE_OK) {
                r = archive_write_add_filter_bzip2(a);
                if (r == ARCHIVE_OK) {
                    archive_write_set_filter_option(a, "bzip2", "compression-level", "9");
                }
            }
            break;
        case ContainerFormat::Xz:
            r = archive_write_set_format_pax_restricted(a);
            if (r == ARCHIVE_OK) {
                r = archive_write_add_filter_xz(a);
                if (r == ARCHIVE_OK) {
                    archive_write_set_filter_option(a, "xz", "compression-level", "9");
                }
            }
            break;
        default:
            Logger::log(LogLevel::Error, "Unsupported output format for writing: " + container_format_to_string(fmt), processor_tag());
            archive_write_free(a);
            return false;
    }
    if (r == ARCHIVE_WARN) {
        Logger::log(LogLevel::Warning, std::string("LIBARCHIVE WARN: ") + archive_error_string(a), processor_tag());
    }
    if (r != ARCHIVE_OK) {
        Logger::log(LogLevel::Error, "Setting format/filter failed: " + std::string(archive_error_string(a)), processor_tag());
        archive_write_free(a);
        return false;
    }

    r = archive_write_open_filename(a, out_path.string().c_str());
    if (r == ARCHIVE_WARN) {
        Logger::log(LogLevel::Warning, std::string("LIBARCHIVE WARN: ") + archive_error_string(a), processor_tag());
    }
    if (r != ARCHIVE_OK) {
        Logger::log(LogLevel::Error, "archive_write_open_filename: " + std::string(archive_error_string(a)), processor_tag());
        archive_write_free(a);
        return false;
    }

    std::error_code ec;
    std::vector<char> buffer(64 * 1024);

    const fs::path root(src_dir);
    std::unordered_map<std::pair<uintmax_t,uintmax_t>, std::string, PairHash> hardlink_map;

    if (fmt == ContainerFormat::Epub) {
        fs::path mimetype_path = fs::path(src_dir) / "mimetype";
        if (fs::exists(mimetype_path)) {

            std::ifstream ifs(mimetype_path, std::ios::binary);
            std::vector<char> buf((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());

            archive_entry* entry = archive_entry_new();
            archive_entry_set_pathname(entry, "mimetype");
            archive_entry_set_size(entry, buf.size());
            archive_entry_set_filetype(entry, AE_IFREG);
            archive_entry_set_perm(entry, 0644);
            archive_entry_set_mtime(entry, 0, 0);

            int rh = archive_write_header(a, entry);
            if (rh != ARCHIVE_OK && rh != ARCHIVE_WARN) {
                Logger::log(LogLevel::Error, "archive_write_header (mimetype): " + std::string(archive_error_string(a)), processor_tag());
                archive_entry_free(entry);
                archive_write_close(a);
                archive_write_free(a);
                return false;
            }
            if (!buf.empty()) {
                la_ssize_t wrote = archive_write_data(a, buf.data(), buf.size());
                if (wrote < 0) {
                    Logger::log(LogLevel::Error, "archive_write_data (mimetype): " + std::string(archive_error_string(a)), processor_tag());
                    archive_entry_free(entry);
                    archive_write_close(a);
                    archive_write_free(a);
                    return false;
                }
            }
            archive_entry_free(entry);
            archive_write_set_format_option(a, "zip", "compression", "deflate");
            archive_write_set_format_option(a, "zip", "compression-level", "9");
        }
    }

    std::vector<fs::path> files;
    for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator(); ++it) {
        std::error_code ec2;
        if (fs::is_regular_file(it->path(), ec2) || fs::is_symlink(it->path(), ec2)) {
            if (fmt == ContainerFormat::Epub && it->path().filename() == "mimetype") continue;
            files.push_back(it->path());
        }
    }
    if (fmt == ContainerFormat::Cbz || fmt == ContainerFormat::Cbt) {
        std::sort(files.begin(), files.end(), [&](const fs::path& a, const fs::path& b) {
            return natural_less_path(a, b, root);
        });
    }

    for (const auto& p : files) {
        const bool is_dir = fs::is_directory(p, ec);
        const bool is_reg = fs::is_regular_file(p, ec);
        const bool is_symlink = fs::is_symlink(p, ec);

        archive_entry* entry = archive_entry_new();
        if (!entry) {
            Logger::log(LogLevel::Error, "archive_entry_new failed", processor_tag());
            archive_write_close(a);
            archive_write_free(a);
            return false;
        }

        std::string rel = rel_path_of(root, p);
        if (rel.empty()) rel = p.filename().generic_string();
        archive_entry_set_pathname(entry, rel.c_str());

        if (is_dir) {
            archive_entry_set_filetype(entry, AE_IFDIR);
            archive_entry_set_perm(entry, 0755);
        } else if (is_reg) {
            archive_entry_set_filetype(entry, AE_IFREG);
            archive_entry_set_perm(entry, 0644);
            std::uintmax_t fsize = fs::file_size(p, ec);
            if (ec) fsize = 0;
            archive_entry_set_size(entry, static_cast<la_int64_t>(fsize));

#ifndef _WIN32
            struct stat st{};
            if (stat(p.c_str(), &st) == 0 && st.st_nlink > 1) {
                auto key = std::make_pair(static_cast<uintmax_t>(st.st_dev), static_cast<uintmax_t>(st.st_ino));
                auto it_hl = hardlink_map.find(key);
                if (it_hl != hardlink_map.end()) {
                    archive_entry_set_hardlink(entry, it_hl->second.c_str());
                    archive_entry_set_size(entry, 0);
                } else {
                    hardlink_map[key] = rel;
                }
            }
#endif
        } else if (is_symlink) {
            archive_entry_set_filetype(entry, AE_IFLNK);
            archive_entry_set_perm(entry, 0777);
            auto target = fs::read_symlink(p, ec);
            if (!ec) {
                archive_entry_set_symlink(entry, target.string().c_str());
            }
        } else {
            archive_entry_free(entry);
            continue;
        }

        r = archive_write_header(a, entry);
        if (r == ARCHIVE_WARN) {
            Logger::log(LogLevel::Warning, std::string("LIBARCHIVE WARN: ") + archive_error_string(a), processor_tag());
        }
        if (r != ARCHIVE_OK) {
            Logger::log(LogLevel::Error, "archive_write_header: " + std::string(archive_error_string(a)) + " for " + rel, processor_tag());
            archive_entry_free(entry);
            archive_write_close(a);
            archive_write_free(a);
            return false;
        }

        if (is_reg) {
            bool skip_data = (archive_entry_hardlink(entry) != nullptr);
            if (!skip_data) {
                std::ifstream ifs(p, std::ios::binary);
                if (!ifs) {
                    Logger::log(LogLevel::Error, "Can't open file for reading: " + p.string(), processor_tag());
                    archive_entry_free(entry);
                    archive_write_close(a);
                    archive_write_free(a);
                    return false;
                }
                while (ifs) {
                    ifs.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                    std::streamsize got = ifs.gcount();
                    if (got > 0) {
                        la_ssize_t wrote = archive_write_data(a, buffer.data(), static_cast<size_t>(got));
                        if (wrote < 0) {
                            Logger::log(LogLevel::Error, "archive_write_data: " + std::string(archive_error_string(a)), processor_tag());
                            archive_entry_free(entry);
                            archive_write_close(a);
                            archive_write_free(a);
                            return false;
                        }
                    }
                }
            }
        }

        archive_entry_free(entry);
    }

    archive_write_close(a);
    archive_write_free(a);
    return true;
}

// --- IProcessor implementation ---

std::optional<ExtractedContent> ArchiveProcessor::prepare_extraction(const std::filesystem::path& input_path) {
    ExtractedContent content;
    content.original_path = input_path;
    content.temp_dir = make_temp_dir();

    // Detect format
    content.format = detect_format(input_path);
    /*
    if (content.format == ContainerFormat::Xpi) {
        std::cerr << "WARNING: Recompressing .xpi will invalidate its digital signature.\n"
                     "You must re-sign the extension to install it.\n" << std::endl;
    }
    if (content.format == ContainerFormat::Apk) {
        std::cerr << "WARNING: Recompressing .apk will invalidate its digital signature.\n"
                     "You must re-sign the APK to install it.\n" << std::endl;
    }*/

    if (!can_read_format(content.format)) {
        Logger::log(LogLevel::Warning, "Unreadable or unrecognized format: " + input_path.filename().string(), processor_tag());
        return content;
    }

    Logger::log(LogLevel::Info, "Extracting archive: " + input_path.filename().string() + " -> " + content.temp_dir.filename().string(), processor_tag());

    if (!extract_with_libarchive(input_path, content.temp_dir)) {
        Logger::log(LogLevel::Error, "Extraction failed for: " + input_path.filename().string(), processor_tag());
        return content;
    }

    for (auto& p : fs::recursive_directory_iterator(content.temp_dir)) {
        std::error_code ec;
        if (fs::is_regular_file(p.path(), ec) || fs::is_symlink(p.path(), ec)) {
            ContainerFormat inner_fmt;
            if (is_archive_file(p.path(), inner_fmt)) {
                Logger::log(LogLevel::Debug, "Found nested archive: " + p.path().string(), processor_tag());
                // Push nested content path to extracted_files; ProcessorExecutor will recurse
                content.extracted_files.push_back(p.path());
            } else {
                content.extracted_files.push_back(p.path());
            }
        }
    }

    Logger::log(
        LogLevel::Debug,
        "Extracted files: " + std::to_string(content.extracted_files.size()),
        processor_tag()
    );

    return content;
}

std::filesystem::path ArchiveProcessor::finalize_extraction(const ExtractedContent& content,
                                                            const ContainerFormat target_format) {
    auto out_fmt = ContainerFormat::Unknown;

    if (can_write_format(content.format)) {
        out_fmt = content.format;
    } else if (target_format != ContainerFormat::Unknown) {
        out_fmt = target_format;
        Logger::log(
            LogLevel::Info,
            "Non writable format (" + container_format_to_string(content.format) + "), recompressing in: " +
            container_format_to_string(out_fmt),
            processor_tag()
        );
    } else {
        Logger::log(
            LogLevel::Info,
            "Non writable format and no alternative format: left intact -> " + content.original_path.filename().string(),
            processor_tag()
        );
        std::error_code ec;
        fs::remove_all(content.temp_dir, ec);
        return {};
    }

    const fs::path src_path(content.original_path);
    const std::string out_ext = "." + container_format_to_string(out_fmt);

    const fs::path tmp_archive = fs::temp_directory_path() /
                                 (src_path.stem().string() + "_tmp" + RandomUtils::random_suffix() + out_ext);

    Logger::log(LogLevel::Info, "Recreating archive: " + tmp_archive.string(), processor_tag());

    if (!create_with_libarchive(content.temp_dir, tmp_archive, out_fmt)) {
        Logger::log(LogLevel::Error, "Archive creation failed: " + tmp_archive.string(), processor_tag());
        fs::remove_all(content.temp_dir);
        throw std::runtime_error("ArchiveProcessor: create_with_libarchive failed");
    }

    std::error_code ec;
    if (!fs::exists(tmp_archive, ec) || ec) {
        Logger::log(LogLevel::Error, "Compressed archive not found: " + tmp_archive.string(), processor_tag());
        fs::remove_all(content.temp_dir);
        throw std::runtime_error("ArchiveProcessor: tmp archive missing");
    }

    std::error_code cleanup_ec;
    fs::remove_all(content.temp_dir, cleanup_ec);
    if (cleanup_ec) {
        Logger::log(LogLevel::Warning, "Can't remove temp dir: " + content.temp_dir.filename().string() + " (" + cleanup_ec.message() + ")", processor_tag());
    } else {
        Logger::log(LogLevel::Debug, "Removed temp dir: " + content.temp_dir.filename().string(), processor_tag());
    }

    return tmp_archive;
}

std::string ArchiveProcessor::get_raw_checksum(const std::filesystem::path& /*file_path*/) const {
    // TODO: define a meaningful checksum for archives if needed (optional)
    return "";
}

} // namespace chisel