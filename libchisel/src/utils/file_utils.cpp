//
// Created by Giuseppe Francione on 17/11/25.
//

#include <filesystem>
#include "../../include/file_utils.hpp"

#include <sstream>

#include "../../include/logger.hpp"
#include "../../include/random_utils.hpp"
#include <system_error>


namespace chisel {
    namespace fs = std::filesystem;

    uint16_t read_le16(const uint8_t* p) {
        return static_cast<uint16_t>(p[0] | (p[1] << 8));
    }

    uint32_t read_le32(const uint8_t* p) {
        return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
    }

    uint64_t read_le64(const uint8_t* p) {
        return static_cast<uint64_t>(read_le32(p)) | (static_cast<uint64_t>(read_le32(p + 4)) << 32);
    }

    void write_le16(uint8_t* p, const uint16_t v) {
        p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF;
    }

    void write_le32(uint8_t* p, const uint32_t v) {
        p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
    }

    void write_le64(uint8_t* p, const uint64_t v) {
        write_le32(p, static_cast<uint32_t>(v)); write_le32(p + 4, static_cast<uint32_t>(v >> 32));
    }

    uint16_t read_be16(const uint8_t* p) {
        return static_cast<uint16_t>((p[0] << 8) | p[1]);
    }

    uint32_t read_be32(const uint8_t* p) {
        return static_cast<uint32_t>((p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
    }

    void write_be16(uint8_t* p, const uint16_t v) {
        p[0] = (v >> 8) & 0xFF; p[1] = v & 0xFF;
    }

    void write_be32(uint8_t* p, const uint32_t v) {
        p[0] = (v >> 24) & 0xFF; p[1] = (v >> 16) & 0xFF; p[2] = (v >> 8) & 0xFF; p[3] = v & 0xFF;
    }

    uint32_t align4(const uint32_t v) {
        return (v + 3) & ~3;
    }

    // format zero-padded index for sorting
    std::string format_index(const size_t index) {
        std::ostringstream oss;
        oss << std::setw(4) << std::setfill('0') << index;
        return oss.str();
    }

    FILE *open_file(const std::filesystem::path &path, const char *mode) {
#ifdef _WIN32
        // On Windows, convert mode to wstring and use _wfopen, which accepts
        // wide-char paths (UTF-16), supporting Unicode and long paths.
        std::wstring wmode;
        for (const char *p = mode; *p; ++p) wmode += static_cast<wchar_t>(*p);

        // get absolute path, required for the long path prefix
        std::error_code ec;
        auto abs_path = std::filesystem::absolute(path, ec);
        if (ec) {
            // fallback to original behavior on error
            return _wfopen(path.wstring().c_str(), wmode.c_str());
        }

        // prepend the magic prefix to bypass MAX_PATH
        std::wstring long_path = L"\\\\?\\" + abs_path.wstring();
        return _wfopen(long_path.c_str(), wmode.c_str());
#else
        return std::fopen(path.string().c_str(), mode);
#endif
    }

    std::filesystem::path make_temp_dir_for(const std::filesystem::path &input_path, const std::string &prefix) {
        // use a common base dir inside temp
        const auto base_tmp = std::filesystem::temp_directory_path() /
                              ("chisel-" + prefix);

        std::error_code ec;
        std::filesystem::create_directories(base_tmp, ec);

        const std::string stem = input_path.stem().string();
        const std::string dir_name = prefix + "_" + stem + "_" + RandomUtils::random_suffix();
        auto dir = base_tmp / dir_name;

        std::filesystem::create_directories(dir, ec);
        if (ec) {
            Logger::log(LogLevel::Error,
                        "Failed to create temp dir: " + dir.string() + " (" + ec.message() + ")",
                        "FileUtils");
        }
        return dir;
    }

    void cleanup_temp_dir(const std::filesystem::path &dir, const std::string_view tag) {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        if (ec) {
            Logger::log(LogLevel::Warning, "Can't remove temp dir: " + dir.string() + " (" + ec.message() + ")", tag);
        } else {
            Logger::log(LogLevel::Debug, "Removed temp dir: " + dir.string(), tag);
        }
    }

    std::string to_lower_copy(std::string s) {
        for (auto &c: s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    bool ensure_parent_dirs(const fs::path &p, std::error_code &ec) {
        const auto parent = p.parent_path();
        if (parent.empty()) return true;
        if (fs::exists(parent, ec)) return !ec;
        fs::create_directories(parent, ec);
        return !ec;
    }

    std::string rel_path_of(const fs::path &root, const fs::path &p) {
        // lexically_relative operates purely on path components and never
        // touches the filesystem, unlike fs::relative() which internally
        // calls weakly_canonical() and therefore resolves symlinks. Since p
        // may itself be (or contain) a symlink, resolving it here would make
        // the computed name reflect the symlink's target instead of its
        // location within root, which is unsafe when used to name archive
        // entries during repacking.
        const auto rel = p.lexically_relative(root);
        std::string s = rel.generic_string();
        return s.empty() || s == "." ? p.filename().generic_string() : s;
    }

    bool natural_less_string(const std::string &sa, const std::string &sb) {
        size_t i = 0;
        size_t j = 0;
        while (i < sa.size() && j < sb.size()) {
            if ((std::isdigit(static_cast<unsigned char>(sa[i])) != 0) && (
                    std::isdigit(static_cast<unsigned char>(sb[j])) != 0)) {
                size_t ia = i;
                size_t jb = j;
                while (ia < sa.size() && (std::isdigit(static_cast<unsigned char>(sa[ia])) != 0)) ++ia;
                while (jb < sb.size() && (std::isdigit(static_cast<unsigned char>(sb[jb])) != 0)) ++jb;

                std::string as = sa.substr(i, ia - i);
                std::string bs = sb.substr(j, jb - j);

                auto strip_leading = [](const std::string &s) -> std::string {
                    size_t k = 0;
                    while (k + 1 < s.size() && s[k] == '0') ++k;
                    return s.substr(k);
                };

                std::string as2 = strip_leading(as);
                std::string bs2 = strip_leading(bs);

                if (as2.size() != bs2.size()) return as2.size() < bs2.size();
                if (as2 != bs2) return as2 < bs2;
                i = ia;
                j = jb;
            } else {
                if (sa[i] != sb[j]) return sa[i] < sb[j];
                ++i;
                ++j;
            }
        }
        return sa.size() < sb.size();
    }

    bool natural_less_path(const fs::path &a, const fs::path &b, const fs::path &root) {
        const std::string sa = rel_path_of(root, a);
        const std::string sb = rel_path_of(root, b);
        return natural_less_string(sa, sb);
    }

    bool sanitize_archive_entry_path(const std::string &entry_name, const fs::path &dest_dir, fs::path &out_path) {
        if (entry_name.empty()) return false;
        // Check for null bytes (poisoning)
        if (entry_name.find('\0') != std::string::npos) return false;

        std::string s = entry_name;
        // Normalize separators
        for (auto &c: s) { if (c == '\\') c = '/'; }
        // Remove absolute path indicators at start
        while (!s.empty() && s.front() == '/') s.erase(s.begin());

        fs::path candidate = fs::path(dest_dir) / fs::path(s).relative_path();

        // Lexically normalize to resolve ".."
        auto normalized = candidate.lexically_normal();
        auto base = fs::path(dest_dir).lexically_normal();

        const auto ns = normalized.string();
        const auto bs = base.string();

        // Check if normalized path still starts with dest_dir
        if (ns.size() < bs.size()) return false;
        if (!ns.starts_with(bs)) return false;

        out_path = normalized;
        return true;
    }
    // helper to write buffer into file
    bool write_file(const std::filesystem::path &path, const std::vector<uint8_t> &buf) {
        std::ofstream out(path, std::ios::binary);
        if (!out) return false;
        out.write(reinterpret_cast<const char*>(buf.data()), buf.size());
        return true;
    }

    bool read_file(const std::filesystem::path &path, std::vector<uint8_t> &buf) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        buf.assign(std::istreambuf_iterator(in), {});
        return true;
    }

    std::vector<uint8_t> read_file(const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) throw std::runtime_error("cannot open file: " + path.string());
        const auto size = file.tellg();
        file.seekg(0, std::ios::beg);
        std::vector<uint8_t> buffer(size);
        if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
            throw std::runtime_error("error reading file: " + path.string());
        }
        return buffer;
    }
} // namespace chisel

