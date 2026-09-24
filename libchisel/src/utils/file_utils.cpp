//
// Created by Giuseppe Francione on 17/11/25.
//

#include <filesystem>
#include "../../include/file_utils.hpp"

#include <sstream>

#include "../../include/logger.hpp"
#include "../../include/random_utils.hpp"
#include <algorithm>
#include <cstring>
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

    uint64_t read_be64(const uint8_t* p) {
        return (static_cast<uint64_t>(read_be32(p)) << 32) | read_be32(p + 4);
    }

    void write_be64(uint8_t* p, const uint64_t v) {
        write_be32(p, static_cast<uint32_t>(v >> 32));
        write_be32(p + 4, static_cast<uint32_t>(v & 0xFFFFFFFFu));
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

        // the stem only makes the folder recognizable, and a long one eats into path length limits
        std::string stem = input_path.stem().string();
        if (stem.size() > 32) {
            std::size_t cut = 32;
            while (cut > 0 && (static_cast<unsigned char>(stem[cut]) & 0xC0) == 0x80) --cut;
            stem.resize(cut);
        }
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

    bool path_is_within(const fs::path &normalized, const fs::path &base) {
        const auto ns = normalized.string();
        const auto bs = base.string();

        if (ns.size() < bs.size()) return false;
        if (!ns.starts_with(bs)) return false;
        // require an exact match or a path-separator boundary right after the
        // prefix, so a sibling like "bs-evil" isn't mistaken for a descendant
        return ns.size() == bs.size() || ns[bs.size()] == '/' || ns[bs.size()] == fs::path::preferred_separator;
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

        if (!path_is_within(normalized, base)) return false;

        out_path = normalized;
        return true;
    }
    // helper to write buffer into file
    bool write_file(const std::filesystem::path &path, const std::vector<uint8_t> &buf) {
        return write_file(path, buf.data(), buf.size());
    }

    bool write_file(const std::filesystem::path &path, const void* data, const std::size_t size) {
        std::ofstream out(path, std::ios::binary);
        if (!out) return false;
        out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
        // closing flushes the buffer, where a full disk shows up
        out.close();
        return !out.fail();
    }

    bool read_file(const std::filesystem::path &path, std::vector<uint8_t> &buf) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        buf.assign(std::istreambuf_iterator(in), {});
        return true;
    }

    bool file_contains(const std::filesystem::path& path, const std::string_view needle) {
        return file_contains(path, {needle});
    }

    bool file_contains(const std::filesystem::path& path, const std::initializer_list<std::string_view> needles,
                       std::uint64_t limit) {
        std::ifstream in(path, std::ios::binary);
        std::size_t longest = 0;
        for (const auto needle : needles) {
            if (needle.empty()) return false;
            longest = std::max(longest, needle.size());
        }
        if (!in || longest == 0) return false;
        std::vector<char> buffer(std::max<std::size_t>(1 << 20, longest * 2));
        std::size_t kept = 0;
        for (;;) {
            const auto want = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size() - kept, limit));
            in.read(buffer.data() + kept, static_cast<std::streamsize>(want));
            const auto got = static_cast<std::size_t>(in.gcount());
            limit -= got;
            const std::size_t filled = kept + got;
            const std::string_view chunk(buffer.data(), filled);
            for (const auto needle : needles) {
                if (chunk.find(needle) != std::string_view::npos) return true;
            }
            if (got == 0 || !in || limit == 0) return false;
            // the tail may hold the start of a match that continues in the next chunk
            kept = std::min(filled, longest - 1);
            std::memmove(buffer.data(), buffer.data() + filled - kept, kept);
        }
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

