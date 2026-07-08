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
#include "file_utils.hpp"
#ifndef _WIN32
#include <sys/stat.h>

#endif

namespace chisel {

namespace fs = std::filesystem;


// --- format detection ---

/**
 * @brief Detects the container format of a file.
 * @param path The path to the file.
 * @return The detected ContainerFormat, or Unknown if not identified.
 */
static ContainerFormat detect_format(const fs::path& path) {
    const std::string mime = MimeDetector::detect(path);
    if (!mime.empty()) {
        const auto it = mime_to_format.find(mime);
        if (it != mime_to_format.end()) return it->second;
    }

    std::string ext = to_lower_copy(path.extension().string());
    if (!ext.empty() && ext.front() == '.') ext.erase(0, 1);
    if (!ext.empty()) {
        if (const auto parsed = parse_container_format(ext)) {
            return *parsed;
        }
        const auto fname = to_lower_copy(path.filename().string());
        if (fname.ends_with(".tar.gz"))  return ContainerFormat::GZip;
        if (fname.ends_with(".tar.bz2")) return ContainerFormat::BZip2;
        if (fname.ends_with(".tar.xz"))  return ContainerFormat::Xz;
        if (fname.ends_with(".tar.zst") || fname.ends_with(".tzst")) return ContainerFormat::Zstd;
    }
    return ContainerFormat::Unknown;
}

/**
 * @brief Checks if a file is a supported archive format.
 * @param path The path to the file.
 * @param fmt_out The detected container format if the file is a readable archive.
 * @return True if the file is a readable archive, false otherwise.
 */
static bool is_archive_file(const fs::path& path, ContainerFormat& fmt_out) {
    fmt_out = detect_format(path);
    return fmt_out != ContainerFormat::Unknown && can_read_format(fmt_out);
}

// --- libarchive extract/create ---

/**
 * @brief Validates that a symlink entry's target stays within the extraction sandbox.
 *
 * A symlink's own path is sanitized separately (see sanitize_archive_entry_path);
 * this additionally validates what it *points to*, since an unvalidated absolute
 * or ".."-laden target lets a crafted archive plant a link to any file on the
 * host filesystem. Relative targets are resolved against the symlink's own
 * directory, matching POSIX symlink semantics.
 *
 * @param raw_target The raw symlink target string from the archive entry.
 * @param symlink_parent_dir The directory that will contain the symlink.
 * @param dest_dir The extraction destination (sandbox root).
 * @return True if the resolved target stays inside dest_dir.
 */
static bool sanitize_symlink_target(const std::string& raw_target,
                                     const fs::path& symlink_parent_dir,
                                     const fs::path& dest_dir) {
    if (raw_target.empty()) return false;
    if (raw_target.find('\0') != std::string::npos) return false;

    std::string t = raw_target;
    for (auto& c : t) { if (c == '\\') c = '/'; }

    const fs::path target_path(t);
    const fs::path candidate = target_path.is_absolute()
        ? target_path
        : symlink_parent_dir / target_path;

    const auto normalized = candidate.lexically_normal();
    const auto base = fs::path(dest_dir).lexically_normal();

    const auto ns = normalized.string();
    const auto bs = base.string();

    if (ns.size() < bs.size()) return false;
    return ns.starts_with(bs);
}

/**
 * @brief Extracts the contents of an archive to a destination directory using libarchive.
 * @param archive_path The path to the archive file.
 * @param dest_dir The directory where contents will be extracted.
 * @return True on successful extraction, false otherwise.
 */
static bool extract_with_libarchive(const fs::path& archive_path, const fs::path& dest_dir) {
    struct archive* a = archive_read_new();
    struct archive_entry* entry = nullptr;

    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);
    archive_read_set_options(a, "hdrcharset=UTF-8");

    #ifdef _WIN32
        int r = archive_read_open_filename_w(a, archive_path.wstring().c_str(), 10240);
    #else
        int r = archive_read_open_filename(a, archive_path.string().c_str(), 10240);
    #endif
    if (r == ARCHIVE_WARN) {
        Logger::log(LogLevel::Warning, std::string("LIBARCHIVE WARN: ") + archive_error_string(a), "ArchiveProcessor");
    }
    if (r != ARCHIVE_OK) {
        Logger::log(LogLevel::Error, "Archive_read_open_filename: " + std::string(archive_error_string(a)), "ArchiveProcessor");
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
            Logger::log(LogLevel::Warning, "Skipping suspicious archive entry (path traversal): " + std::string(current), "ArchiveProcessor");
            archive_read_data_skip(a);
            continue;
        }

        if (!ensure_parent_dirs(out_path, ec)) {
            Logger::log(LogLevel::Error, "Can't create folder for: " + out_path.string(), "ArchiveProcessor");
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
                if (!sanitize_symlink_target(link_target, out_path.parent_path(), dest_dir)) {
                    Logger::log(LogLevel::Warning,
                                "Skipping symlink with unsafe target (escapes extraction sandbox): " +
                                std::string(current) + " -> " + link_target,
                                "ArchiveProcessor");
                } else {
                    std::error_code rc;
                    fs::create_directories(out_path.parent_path(), rc);
#ifdef _WIN32
                    std::error_code tmp_ec;
                    fs::create_symlink(fs::path(link_target), out_path, tmp_ec);
                    (void)tmp_ec;
#else
                    std::error_code sce;
                    fs::create_symlink(fs::path(link_target), out_path, sce);
#endif
                }
            }
            archive_read_data_skip(a);
            continue;
        }

        // override the pathname to match the sanitized path
        archive_entry_set_pathname(entry, out_path.string().c_str());

        // extract directly with libarchive preserving time and permissions
        constexpr int extract_flags = ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM | ARCHIVE_EXTRACT_SECURE_NODOTDOT;
        const int ext_r = archive_read_extract(a, entry, extract_flags);

        if (ext_r != ARCHIVE_OK) {
            Logger::log(LogLevel::Error, "extraction failed: " + std::string(archive_error_string(a)), "ArchiveProcessor");
            archive_read_free(a);
            return false;
        }
    }

    if (r != ARCHIVE_EOF) {
        Logger::log(LogLevel::Error, "Error during iteration: " + std::string(archive_error_string(a)), "ArchiveProcessor");
        archive_read_free(a);
        return false;
    }

    archive_read_free(a);
    return true;
}

/**
 * @brief A hash function for std::pair, used for the hardlink map.
 */
struct PairHash {
    template <class T1, class T2>
    std::size_t operator()(const std::pair<T1,T2>& p) const noexcept {
        auto h1 = std::hash<T1>{}(p.first);
        auto h2 = std::hash<T2>{}(p.second);
        return h1 ^ (h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
    }
};

/**
 * @brief Creates an archive from a source directory using libarchive.
 * @param src_dir The directory containing the files to be archived.
 * @param out_path The path to the output archive file.
 * @param fmt The target container format for the new archive.
 * @return True on successful creation, false otherwise.
 */
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
        case ContainerFormat::Zstd:
            r = archive_write_set_format_pax_restricted(a);
            if (r == ARCHIVE_OK) {
                r = archive_write_add_filter_zstd(a);
                if (r == ARCHIVE_OK) {
                    archive_write_set_filter_option(a, "zstd", "compression-level", "22");
                }
            }
            break;

        case ContainerFormat::Iso:
            r = archive_write_set_format_iso9660(a);
            if (r == ARCHIVE_OK) {
                archive_write_set_format_option(a, "iso9660", "joliet", "1");
                archive_write_set_format_option(a, "iso9660", "rockridge", "1");
                archive_write_set_format_option(a, "iso9660", "pad", "0");
            }
            break;

        case ContainerFormat::Cpio:
            r = archive_write_set_format_cpio(a);
            break;

        case ContainerFormat::Ar:
            r = archive_write_set_format_ar_bsd(a);
            break;
        default:
            Logger::log(LogLevel::Error, "Unsupported output format for writing: " + container_format_to_string(fmt), "ArchiveProcessor");
            archive_write_free(a);
            return false;
    }
    if (r == ARCHIVE_WARN) {
        Logger::log(LogLevel::Warning, std::string("LIBARCHIVE WARN: ") + archive_error_string(a), "ArchiveProcessor");
    }
    if (r != ARCHIVE_OK) {
        Logger::log(LogLevel::Error, "Setting format/filter failed: " + std::string(archive_error_string(a)), "ArchiveProcessor");
        archive_write_free(a);
        return false;
    }

    #ifdef _WIN32
        r = archive_write_open_filename_w(a, out_path.wstring().c_str());
    #else
        r = archive_write_open_filename(a, out_path.string().c_str());
    #endif
    if (r == ARCHIVE_WARN) {
        Logger::log(LogLevel::Warning, std::string("LIBARCHIVE WARN: ") + archive_error_string(a), "ArchiveProcessor");
    }
    if (r != ARCHIVE_OK) {
        Logger::log(LogLevel::Error, "Archive_write_open_filename: " + std::string(archive_error_string(a)), "ArchiveProcessor");
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
                Logger::log(LogLevel::Error, "Archive_write_header (mimetype): " + std::string(archive_error_string(a)), "ArchiveProcessor");
                archive_entry_free(entry);
                archive_write_close(a);
                archive_write_free(a);
                return false;
            }
            if (!buf.empty()) {
                la_ssize_t wrote = archive_write_data(a, buf.data(), buf.size());
                if (wrote < 0) {
                    Logger::log(LogLevel::Error, "Archive_write_data (mimetype): " + std::string(archive_error_string(a)), "ArchiveProcessor");
                    archive_entry_free(entry);
                    archive_write_close(a);
                    archive_write_free(a);
                    return false;
                }
            }
            archive_write_finish_entry(a); // finish this entry
            archive_entry_free(entry);
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
            Logger::log(LogLevel::Error, "Archive_entry_new failed", "ArchiveProcessor");
            archive_write_close(a);
            archive_write_free(a);
            return false;
        }

        std::string rel = rel_path_of(root, p);
        if (rel.empty()) rel = p.filename().generic_string();
        archive_entry_set_pathname(entry, rel.c_str());

        if (is_symlink) {
            // must be checked before is_dir/is_reg: fs::is_directory() and
            // fs::is_regular_file() both follow symlinks, so a symlink
            // pointing to a file or directory would otherwise be silently
            // dereferenced here, reading/naming entries after its target
            // instead of preserving it as a symlink.
            archive_entry_set_filetype(entry, AE_IFLNK);
            archive_entry_set_perm(entry, 0777);
            auto target = fs::read_symlink(p, ec);
            if (!ec) {
                archive_entry_set_symlink(entry, target.string().c_str());
            }
        } else if (is_dir) {
            archive_entry_set_filetype(entry, AE_IFDIR);
            archive_entry_set_perm(entry, 0755);
        } else if (is_reg) {
            archive_entry_set_filetype(entry, AE_IFREG);
            std::uintmax_t fsize = fs::file_size(p, ec);
            if (ec) fsize = 0;
            archive_entry_set_size(entry, static_cast<la_int64_t>(fsize));

#ifndef _WIN32
            // posix: read real stats for permissions and modified time
            struct stat st{};
            if (stat(p.c_str(), &st) == 0) {
                archive_entry_set_perm(entry, st.st_mode);
                archive_entry_set_mtime(entry, st.st_mtime, 0);

                if (st.st_nlink > 1) {
                    auto key = std::make_pair(static_cast<uintmax_t>(st.st_dev), static_cast<uintmax_t>(st.st_ino));
                    auto it_hl = hardlink_map.find(key);
                    if (it_hl != hardlink_map.end()) {
                        archive_entry_set_hardlink(entry, it_hl->second.c_str());
                        archive_entry_set_size(entry, 0);
                    } else {
                        hardlink_map[key] = rel;
                    }
                }
            } else {
                archive_entry_set_perm(entry, 0644);
            }
#else
            // windows fallback
            archive_entry_set_perm(entry, 0644);
            auto ftime = fs::last_write_time(p, ec);
            if (!ec) {
                auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
                std::time_t tt = std::chrono::system_clock::to_time_t(sctp);
                archive_entry_set_mtime(entry, tt, 0);
            }
#endif
        } else {
            archive_entry_free(entry);
            continue;
        }

        r = archive_write_header(a, entry);
        if (r == ARCHIVE_WARN) {
            Logger::log(LogLevel::Warning, std::string("LIBARCHIVE WARN: ") + archive_error_string(a), "ArchiveProcessor");
        }
        if (r != ARCHIVE_OK) {
            Logger::log(LogLevel::Error, "Archive_write_header: " + std::string(archive_error_string(a)) + " for " + rel, "ArchiveProcessor");
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
                    Logger::log(LogLevel::Error, "Can't open file for reading: " + p.string(), "ArchiveProcessor");
                    archive_entry_free(entry);
                    archive_write_close(a);
                    archive_write_free(a);
                    return false;
                }
                while (ifs) {
                    ifs.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                    std::streamsize got = ifs.gcount();
                    if (got > 0) {
                        la_ssize_t wrote = archive_write_data(a, buffer.data(), static_cast<std::size_t>(got));
                        if (wrote < 0) {
                            Logger::log(LogLevel::Error, "Archive_write_data: " + std::string(archive_error_string(a)), "ArchiveProcessor");
                            archive_entry_free(entry);
                            archive_write_close(a);
                            archive_write_free(a);
                            return false;
                        }
                    }
                }
            }
        }

        archive_write_finish_entry(a); // finish this entry
        archive_entry_free(entry);
    }

    archive_write_close(a);
    archive_write_free(a);
    return true;
}

// --- IProcessor implementation ---

std::optional<ExtractedContent> ArchiveProcessor::prepare_extraction(const std::filesystem::path& input_path) {
    Logger::log(LogLevel::Debug, "Entering prepare_extraction for " + input_path.string(), get_name());

    ExtractedContent content;
    content.original_path = input_path;
    content.temp_dir = chisel::make_temp_dir_for(input_path, "archive");

    // Detect format
    content.format = detect_format(input_path);

    if (!can_read_format(content.format)) {
        Logger::log(LogLevel::Warning, "Unreadable or unrecognized format: " + input_path.filename().string(), get_name());
        cleanup_temp_dir(content.temp_dir, get_name());
        return std::nullopt;
    }

    if (!extract_with_libarchive(input_path, content.temp_dir)) {
        Logger::log(LogLevel::Error, "Extraction failed for: " + input_path.filename().string(), get_name());
        cleanup_temp_dir(content.temp_dir, get_name());
        return std::nullopt;
    }

    for (auto& p : fs::recursive_directory_iterator(content.temp_dir)) {
        std::error_code ec;
        if (fs::is_regular_file(p.path(), ec) || fs::is_symlink(p.path(), ec)) {
            ContainerFormat inner_fmt;
            if (is_archive_file(p.path(), inner_fmt)) {
                Logger::log(LogLevel::Debug, "Found nested archive: " + p.path().string(), get_name());
                // Push nested content path to extracted_files; ProcessorExecutor will recurse
                content.extracted_files.push_back(p.path());
            } else {
                content.extracted_files.push_back(p.path());
            }
        }
    }

    Logger::log(
        LogLevel::Info,
        "Extracted files: " + std::to_string(content.extracted_files.size()),
        get_name()
    );

    Logger::log(LogLevel::Debug, "Exiting prepare_extraction for " + input_path.string(), get_name());
    return content;
}

std::filesystem::path ArchiveProcessor::finalize_extraction(const ExtractedContent& content, const ProcessingOptions &options) {
    Logger::log(LogLevel::Debug, "Entering finalize_extraction for " + content.original_path.string(), get_name());

    const auto out_fmt = content.format;
    const fs::path src_path(content.original_path);
    const std::string out_ext = "." + container_format_to_string(out_fmt);

    const fs::path tmp_archive = fs::temp_directory_path() /
                                 (src_path.stem().string() + "_tmp" + RandomUtils::random_suffix() + out_ext);

    if (!create_with_libarchive(content.temp_dir, tmp_archive, out_fmt)) {
        Logger::log(LogLevel::Error, "Archive creation failed: " + tmp_archive.string(), get_name());
        fs::remove_all(content.temp_dir);
        // create_with_libarchive may have already opened/partially written tmp_archive
        // before failing mid-stream; don't leak the truncated file.
        std::error_code rm_ec;
        fs::remove(tmp_archive, rm_ec);
        throw std::runtime_error("ArchiveProcessor: create_with_libarchive failed");
    }

    std::error_code ec;
    if (!fs::exists(tmp_archive, ec) || ec) {
        Logger::log(LogLevel::Error, "Compressed archive not found: " + tmp_archive.string(), get_name());
        fs::remove_all(content.temp_dir);
        throw std::runtime_error("ArchiveProcessor: tmp archive missing");
    }

    chisel::cleanup_temp_dir(content.temp_dir, get_name());

    Logger::log(LogLevel::Debug, "Exiting finalize_extraction for " + tmp_archive.string(), get_name());
    return tmp_archive;
}

std::string ArchiveProcessor::get_raw_checksum(const std::filesystem::path& /*file_path*/) const {
    // TODO: define a meaningful checksum for archives if needed (optional)
    return "";
}

} // namespace chisel