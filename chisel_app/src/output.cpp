#include "chisel_app/output.hpp"
#include "chisel_app/paths.hpp"

#include "file_utils.hpp"

#include <cstdio>
#include <random>

#ifdef _WIN32
#define NOGDI
#define NOUSER
#include <windows.h>
#else
#include <cerrno>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace chisel_app {

    namespace {
        std::string random_hex() {
            static thread_local std::mt19937_64 rng{std::random_device{}()};
            char text[17];
            std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(rng()));
            return text;
        }

#ifdef _WIN32
        std::error_code last_error() {
            return {static_cast<int>(GetLastError()), std::system_category()};
        }
#endif

        // a rename on one volume, a copy otherwise
        std::error_code move_file(const fs::path& from, const fs::path& to) {
            std::error_code ec;
            fs::rename(from, to, ec);
            if (!ec) return ec;
            ec.clear();
            fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
            std::error_code rm_ec;
            if (ec) {
                fs::remove(to, rm_ec);
                return ec;
            }
            fs::remove(from, rm_ec);
            return ec;
        }
    } // namespace

    fs::path side_by_side_name(const fs::path& original) {
        auto name = original.stem();
        name += ".compressed";
        name += original.extension();
        return original.parent_path() / name;
    }

    fs::path backup_name(const fs::path& original) {
        auto name = original.stem();
        name += ".original";
        name += original.extension();
        return original.parent_path() / name;
    }

    fs::path unique_path(const fs::path& wanted) {
        std::error_code ec;
        if (!fs::exists(fs::symlink_status(wanted, ec))) return wanted;
        const auto dir = wanted.parent_path();
        const auto stem = wanted.stem();
        const auto ext = wanted.extension();
        for (unsigned n = 1;; ++n) {
            auto name = stem;
            name += "-" + std::to_string(n);
            name += ext;
            auto candidate = dir / name;
            if (!fs::exists(fs::symlink_status(candidate, ec))) return candidate;
        }
    }

    std::error_code rename_no_replace(const fs::path& from, const fs::path& to) {
#ifdef _WIN32
        if (MoveFileExW(from.c_str(), to.c_str(), 0)) return {};
        return last_error();
#elif defined(__APPLE__)
        if (::renamex_np(from.c_str(), to.c_str(), RENAME_EXCL) == 0) return {};
        return {errno, std::generic_category()};
#else
        if (::link(from.c_str(), to.c_str()) == 0) {
            ::unlink(from.c_str());
            return {};
        }
        if (errno == EEXIST) return {EEXIST, std::generic_category()};
        // file systems without hard links (fat, exfat): check, then rename
        std::error_code ec;
        if (fs::exists(fs::symlink_status(to, ec))) return {EEXIST, std::generic_category()};
        fs::rename(from, to, ec);
        return ec;
#endif
    }

    PlaceResult place_result(const OutputMode mode, const fs::path& produced, const fs::path& original) {
        PlaceResult r;
        std::error_code ec;
        // next to the original first, so that the steps touching it are renames on one volume
        const auto sibling = original.parent_path() / (".chisel-" + random_hex() + ".tmp");
        if ((ec = move_file(produced, sibling))) {
            fs::remove(produced, ec);
            r.error = "can't write next to the original (" + ec.message() + ")";
            return r;
        }
        // the result takes the original's permissions, owner and extended attributes, as in place
        chisel::copy_file_metadata(original, sibling);

        if (mode == OutputMode::SideBySide) {
            for (int attempt = 0; attempt < 5; ++attempt) {
                r.result = unique_path(side_by_side_name(original));
                ec = rename_no_replace(sibling, r.result);
                if (!ec) return r;
                if (!fs::exists(fs::symlink_status(r.result))) break;
            }
            fs::remove(sibling, ec);
            r.error = "can't save " + to_utf8(r.result.filename()) + " (" + ec.message() + ")";
            r.result.clear();
            return r;
        }

        r.backup = unique_path(backup_name(original));
#ifdef _WIN32
        // the original becomes the backup and its attributes, access control list and streams go to the result
        if (ReplaceFileW(original.c_str(), sibling.c_str(), r.backup.c_str(),
                         REPLACEFILE_IGNORE_MERGE_ERRORS | REPLACEFILE_IGNORE_ACL_ERRORS, nullptr, nullptr)) {
            r.result = original;
            return r;
        }
        if (GetLastError() == ERROR_UNABLE_TO_MOVE_REPLACEMENT_2) {
            // the original is already the backup and its name is free
            if (MoveFileExW(sibling.c_str(), original.c_str(), 0)) {
                r.result = original;
                return r;
            }
            ec = last_error();
            MoveFileExW(r.backup.c_str(), original.c_str(), 0);
            fs::remove(sibling, ec);
            r.error = "can't put the result in place (" + ec.message() + ")";
            r.backup.clear();
            return r;
        }
#endif
        const auto restore_acl = chisel::keep_access_control_list(original);
        if ((ec = rename_no_replace(original, r.backup))) {
            std::error_code rm_ec;
            fs::remove(sibling, rm_ec);
            r.error = "can't rename the original to " + to_utf8(r.backup.filename()) + " (" + ec.message() + ")";
            r.backup.clear();
            return r;
        }
        if ((ec = rename_no_replace(sibling, original))) {
            std::error_code back_ec = rename_no_replace(r.backup, original);
            std::error_code rm_ec;
            fs::remove(sibling, rm_ec);
            r.error = "can't put the result in place (" + ec.message() + ")";
            if (back_ec) r.error += "; the original is still " + to_utf8(r.backup.filename());
            else r.backup.clear();
            return r;
        }
        restore_acl(original);
        r.result = original;
        return r;
    }

    bool folder_writable(const fs::path& folder) {
#ifdef _WIN32
        (void)folder;
        return true;
#else
        return ::access(folder.c_str(), W_OK) == 0;
#endif
    }

    fs::path make_temp_folder(const fs::path& parent, const std::string& tag) {
        for (int attempt = 0; attempt < 5; ++attempt) {
            auto dir = parent / ("." + tag + "-" + random_hex().substr(0, 8));
            std::error_code ec;
            if (fs::create_directory(dir, ec) && !ec) {
#ifdef _WIN32
                SetFileAttributesW(dir.c_str(), FILE_ATTRIBUTE_HIDDEN);
#endif
                return dir;
            }
            if (!fs::exists(parent, ec)) break;
        }
        return {};
    }

} // namespace chisel_app
