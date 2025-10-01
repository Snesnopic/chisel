//
// Created by Giuseppe Francione on 19/09/25.
//

#include "../containers/archive_handler.hpp"
#include "../utils/logger.hpp"
#include "../utils/file_type.hpp"
#include "../utils/archive_formats.hpp"
#include <archive.h>
#include <archive_entry.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <optional>
#include <system_error>
#include <chrono>
#include <random>

namespace fs = std::filesystem;

namespace {

std::string make_temp_dir() {
    const auto base = fs::temp_directory_path();
    const auto now  = std::chrono::steady_clock::now().time_since_epoch().count();
    std::random_device rd;
    std::mt19937_64 gen(rd());
    const auto rnd = gen();

    fs::path dir = base / ("monolith_" + std::to_string(now) + "_" + std::to_string(rnd));
    fs::create_directories(dir);
    return dir.string();
}

std::string to_lower_copy(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string rel_path_of(const fs::path& root, const fs::path& p) {
    const auto rel = fs::relative(p, root);
    // Normalize to forward slashes for archives
    std::string s = rel.generic_string();
    return s.empty() ? p.filename().generic_string() : s;
}

std::string output_extension_for(const ContainerFormat fmt) {
    switch (fmt) {
        case ContainerFormat::Zip:      return ".zip";
        case ContainerFormat::Tar:      return ".tar";
        case ContainerFormat::GZip:     return ".tar.gz";  // implemented as tar + gzip filter
        case ContainerFormat::BZip2:    return ".tar.bz2"; // tar + bzip2 filter
        case ContainerFormat::Xz:       return ".tar.xz";  // tar + xz filter
        case ContainerFormat::SevenZip: return ".7z";
        case ContainerFormat::Wim:      return ".wim";
        case ContainerFormat::Rar:      return ".rar";
        default:                      return ".zip";
    }
}

bool ensure_parent_dirs(const fs::path& p, std::error_code& ec) {
    const auto parent = p.parent_path();
    if (parent.empty()) return true;
    if (fs::exists(parent, ec)) return !ec;
    fs::create_directories(parent, ec);
    return !ec;
}

} // namespace

// ---------- ArchiveHandler public methods ----------

ContainerJob ArchiveHandler::prepare(const std::string &archive_path) {
    ContainerJob job;
    job.original_path = archive_path;
    job.temp_dir = make_temp_dir();

    // detect format
    job.format = detect_format(archive_path);

    if (!can_read_format(job.format)) {
        Logger::log(LogLevel::WARNING, "Unreadable or unrecognized format: " + archive_path, "ArchiveHandler");
        return job;
    }

    Logger::log(LogLevel::INFO, "Extracting archive: " + archive_path + " -> " + job.temp_dir, "ArchiveHandler");

    // extract with libarchive
    if (!extract_with_libarchive(archive_path, job.temp_dir)) {
        Logger::log(LogLevel::ERROR, "Extraction failed for: " + archive_path, "ArchiveHandler");
        return job;
    }

    // scan extracted files and identify nexted archives
    for (auto& p : fs::recursive_directory_iterator(job.temp_dir)) {
        if (!fs::is_regular_file(p.path())) continue;

        ContainerFormat inner_fmt;
        if (is_archive_file(p.path().string(), inner_fmt)) {
            Logger::log(LogLevel::DEBUG, "Found nested archive: " + p.path().string(), "ArchiveHandler");
            job.children.push_back(prepare(p.path().string()));
        } else {
            job.file_list.push_back(p.path().string());
        }
    }

    Logger::log(
        LogLevel::DEBUG,
        "Extracted files: " + std::to_string(job.file_list.size()) +
        " | Nested archives: " + std::to_string(job.children.size()),
        "ArchiveHandler"
    );

    return job;
}

bool ArchiveHandler::finalize(const ContainerJob &job, Settings& settings) {
    // finalize children first
    for (const auto& child : job.children) {
        if (!finalize(child, settings)) {
            Logger::log(LogLevel::ERROR, "Finalize failed per nested archive: " + child.original_path, "ArchiveHandler");
            return false;
        }
    }

    // determine output format
    auto out_fmt = ContainerFormat::Unknown;

    if (can_write_format(job.format)) {
        out_fmt = job.format;
    } else if (settings.unencodable_target_format.has_value()) {
        out_fmt = settings.unencodable_target_format.value();
        Logger::log(
            LogLevel::INFO,
            "Non writable format (" + container_format_to_string(job.format) + "), recompressing in: " +
            container_format_to_string(out_fmt),
            "ArchiveHandler"
        );
    } else {
        Logger::log(
            LogLevel::INFO,
            "Non writable format and no alternative format: left intact -> " + job.original_path,
            "ArchiveHandler"
        );
        // clean temp folder
        std::error_code ec;
        fs::remove_all(job.temp_dir, ec);
        return true;
    }

    // build temp output path
    const fs::path src_path(job.original_path);
    const std::string out_ext = output_extension_for(out_fmt);

    const fs::path tmp_archive = src_path.parent_path() /
                           (src_path.stem().string() + "_tmp" + out_ext);

    Logger::log(
        LogLevel::INFO,
        "Recreating archive: " + tmp_archive.string(),
        "ArchiveHandler"
    );

    // create archive with libarchive
    if (!create_with_libarchive(job.temp_dir, tmp_archive.string(), out_fmt)) {
        Logger::log(LogLevel::ERROR, "Archive creation failed: " + tmp_archive.string(), "ArchiveHandler");
        return false;
    }

    // post-compression check and substitution
    std::error_code ec;
    if (!fs::exists(tmp_archive, ec) || ec) {
        Logger::log(LogLevel::ERROR, "Compressed archive not found: " + tmp_archive.string(), "ArchiveHandler");
        return false;
    }

    auto orig_size = fs::file_size(job.original_path, ec);
    if (ec) orig_size = 0;
    auto new_size  = fs::file_size(tmp_archive, ec);
    if (ec) new_size = 0;

    if (new_size == 0) {
        Logger::log(LogLevel::WARNING, "Empty archive: " + tmp_archive.string(), "ArchiveHandler");
    }

    if (new_size > 0 && (orig_size == 0 || new_size < orig_size)) {
        fs::path final_path = job.original_path;

        // if extension changes, correctly rename
        if (final_path.extension().string() != out_ext) {

            final_path.replace_extension("");
            final_path += out_ext;
        }

        fs::rename(tmp_archive, final_path, ec);
        if (ec) {
            Logger::log(LogLevel::ERROR, "Renaming archive failed: " + final_path.string() + " (" + ec.message() + ")", "ArchiveHandler");
            return false;
        }
        Logger::log(
            LogLevel::INFO,
            "Optimized archive: " + final_path.string() +
            " (" + std::to_string(orig_size) + " -> " + std::to_string(new_size) + " bytes)",
            "ArchiveHandler"
        );
    } else {
        fs::remove(tmp_archive, ec);
        Logger::log(LogLevel::DEBUG, "No improvement for: " + job.original_path, "ArchiveHandler");
    }

    // clean temp dir
    fs::remove_all(job.temp_dir, ec);
    if (ec) {
        Logger::log(LogLevel::WARNING, "Can't remove temp dir: " + job.temp_dir + " (" + ec.message() + ")", "ArchiveHandler");
    } else {
        Logger::log(LogLevel::DEBUG, "Removed temp dir: " + job.temp_dir, "ArchiveHandler");
    }

    return true;
}

// ---------- ArchiveHandler private helpers ----------

ContainerFormat ArchiveHandler::detect_format(const std::string& path) {
    // try mime
    const std::string mime = detect_mime_type(path);
    if (!mime.empty()) {
        auto it = mime_to_format.find(mime);
        if (it != mime_to_format.end()) return it->second;
    }

    // extension fallback
    std::string ext = to_lower_copy(fs::path(path).extension().string());
    if (!ext.empty() && ext.front() == '.') ext.erase(0, 1);
    if (!ext.empty()) {
        if (auto parsed = parse_container_format(ext)) {
            return *parsed;
        }
        // double extension support
        const auto fname = to_lower_copy(fs::path(path).filename().string());
        if (fname.ends_with(".tar.gz"))  return ContainerFormat::GZip;
        if (fname.ends_with(".tar.bz2")) return ContainerFormat::BZip2;
        if (fname.ends_with(".tar.xz"))  return ContainerFormat::Xz;
    }

    return ContainerFormat::Unknown;
}

bool ArchiveHandler::is_archive_file(const std::string& path, ContainerFormat& fmt_out) {
    fmt_out = detect_format(path);
    return fmt_out != ContainerFormat::Unknown && can_read_format(fmt_out);
}

bool ArchiveHandler::extract_with_libarchive(const std::string& archive_path, const std::string& dest_dir) {
    struct archive* a = archive_read_new();
    struct archive_entry* entry = nullptr;

    // enable all filters and formats
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);
    archive_read_set_options(a, "hdrcharset=UTF-8");

    int r = archive_read_open_filename(a, archive_path.c_str(), 10240);
    if (r == ARCHIVE_WARN) {
        Logger::log(LogLevel::WARNING, std::string("LIBARCHIVE WARN: ") + archive_error_string(a), "ArchiveHandler");
    }
    if (r != ARCHIVE_OK) {
        Logger::log(LogLevel::ERROR, "archive_read_open_filename: " + std::string(archive_error_string(a)), "ArchiveHandler");
        archive_read_free(a);
        return false;
    }


    std::error_code ec;
    std::vector<char> buffer;
    buffer.resize(64 * 1024);

    while ((r = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
        const char* current = archive_entry_pathname(entry);
        if (!current) {
            archive_read_data_skip(a);
            continue;
        }

        fs::path out_path = fs::path(dest_dir) / fs::path(current).relative_path();
        if (!ensure_parent_dirs(out_path, ec)) {
            Logger::log(LogLevel::ERROR, "Can't create folder for: " + out_path.string(), "ArchiveHandler");
            archive_read_data_skip(a);
            continue;
        }

        // directory
        if (archive_entry_filetype(entry) == AE_IFDIR) {
            fs::create_directories(out_path, ec);
            archive_read_data_skip(a);
            continue;
        }

        // file
        std::ofstream ofs(out_path, std::ios::binary);
        if (!ofs) {
            Logger::log(LogLevel::ERROR, "Can't open file in write mode: " + out_path.string(), "ArchiveHandler");
            archive_read_data_skip(a);
            continue;
        }

        la_ssize_t size_read = 0;
        while ((size_read = archive_read_data(a, buffer.data(), buffer.size())) > 0) {
            ofs.write(buffer.data(), static_cast<std::streamsize>(size_read));
        }
        ofs.close();

        if (size_read < 0) {
            Logger::log(LogLevel::ERROR, "Error reading data: " + std::string(archive_error_string(a)), "ArchiveHandler");
            archive_read_free(a);
            return false;
        }
    }

    if (r != ARCHIVE_EOF) {
        Logger::log(LogLevel::ERROR, "Error during iteration: " + std::string(archive_error_string(a)), "ArchiveHandler");
        archive_read_free(a);
        return false;
    }

    archive_read_free(a);
    return true;
}



bool ArchiveHandler::create_with_libarchive(const std::string& src_dir, const std::string& out_path, ContainerFormat fmt) {

    archive* a = archive_write_new();
    if (!a) return false;

    int r = ARCHIVE_OK;

    switch (fmt) {
        case ContainerFormat::Zip:
            r = archive_write_set_format_zip(a);
            if (r == ARCHIVE_OK) {
                archive_write_set_filter_option(a, "deflate", "compression-level", "9");
            }
            break;
        case ContainerFormat::Tar:
            r = archive_write_set_format_pax_restricted(a); // standard tar
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
            Logger::log(LogLevel::ERROR, "Unsupported output format for writing: " + container_format_to_string(fmt), "ArchiveHandler");
            archive_write_free(a);
            return false;
    }
    if (r == ARCHIVE_WARN) {
        Logger::log(LogLevel::WARNING, std::string("LIBARCHIVE WARN: ") + archive_error_string(a), "ArchiveHandler");
    }
    if (r != ARCHIVE_OK) {
        Logger::log(LogLevel::ERROR, "Setting format/filter failed: " + std::string(archive_error_string(a)), "ArchiveHandler");
        archive_write_free(a);
        return false;
    }

    // open output file
    r = archive_write_open_filename(a, out_path.c_str());
    if (r == ARCHIVE_WARN) {
        Logger::log(LogLevel::WARNING, std::string("LIBARCHIVE WARN: ") + archive_error_string(a), "ArchiveHandler");
    }
    if (r != ARCHIVE_OK) {
        Logger::log(LogLevel::ERROR, "archive_write_open_filename: " + std::string(archive_error_string(a)), "ArchiveHandler");
        archive_write_free(a);
        return false;
    }

    // iterate source files recursively
    std::error_code ec;
    std::vector<char> buffer;
    buffer.resize(64 * 1024);

    const fs::path root(src_dir);

    // map to track hardlinks by (dev, ino)
    std::unordered_map<std::pair<uintmax_t,uintmax_t>, std::string, pair_hash> hardlink_map;

    for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator(); ++it) {
        const fs::path p = it->path();
        const bool is_dir = fs::is_directory(p, ec);
        const bool is_reg = fs::is_regular_file(p, ec);
        const bool is_symlink = fs::is_symlink(p, ec); // added: detect symlink

        archive_entry* entry = archive_entry_new();
        if (!entry) {
            Logger::log(LogLevel::ERROR, "archive_entry_new failed", "ArchiveHandler");
            archive_write_close(a);
            archive_write_free(a);
            return false;
        }

        // relative name in archive
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

            // check for hardlink
            struct stat st{};
            if (stat(p.c_str(), &st) == 0 && st.st_nlink > 1) {
                auto key = std::make_pair(static_cast<uintmax_t>(st.st_dev), static_cast<uintmax_t>(st.st_ino));
                auto it_hl = hardlink_map.find(key);
                if (it_hl != hardlink_map.end()) {
                    archive_entry_set_hardlink(entry, it_hl->second.c_str());
                    archive_entry_set_size(entry, 0); // no data written
                } else {
                    hardlink_map[key] = rel;
                }
            }
        } else if (is_symlink) {
            // handle symlink
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

        // write header
        r = archive_write_header(a, entry);
        if (r == ARCHIVE_WARN) {
            Logger::log(LogLevel::WARNING, std::string("LIBARCHIVE WARN: ") + archive_error_string(a), "ArchiveHandler");
        }
        if (r != ARCHIVE_OK) {
            Logger::log(LogLevel::ERROR, "archive_write_header: " + std::string(archive_error_string(a)) + " for " + rel, "ArchiveHandler");
            archive_entry_free(entry);
            archive_write_close(a);
            archive_write_free(a);
            return false;
        }

        // write file contents
        if (is_reg) {
            bool skip_data = (archive_entry_hardlink(entry) != nullptr); // skip data if hardlink
            if (!skip_data) {
                std::ifstream ifs(p, std::ios::binary);
                if (!ifs) {
                    Logger::log(LogLevel::ERROR, "Can't open file for reading: " + p.string(), "ArchiveHandler");
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
                            Logger::log(LogLevel::ERROR, "archive_write_data: " + std::string(archive_error_string(a)), "ArchiveHandler");
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

    // close archive
    archive_write_close(a);
    archive_write_free(a);
    return true;
}