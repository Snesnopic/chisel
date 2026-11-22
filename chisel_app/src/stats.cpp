#include "chisel_app/stats.hpp"

#include <charconv>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <set>

namespace fs = std::filesystem;

namespace chisel_app {

    namespace {
        // format version first, so the layout can change later
        constexpr std::string_view kHeader = "chisel-stats\t1";

        std::string escape(const std::string_view text) {
            std::string out;
            for (const char c : text) {
                switch (c) {
                    case '\\': out += "\\\\"; break;
                    case '\t': out += "\\t"; break;
                    case '\n': out += "\\n"; break;
                    case '\r': out += "\\r"; break;
                    default: out += c;
                }
            }
            return out;
        }

        std::string unescape(const std::string_view text) {
            std::string out;
            for (std::size_t i = 0; i < text.size(); ++i) {
                if (text[i] != '\\' || i + 1 == text.size()) {
                    out += text[i];
                    continue;
                }
                const char next = text[++i];
                out += next == 't' ? '\t' : next == 'n' ? '\n' : next == 'r' ? '\r' : next;
            }
            return out;
        }

        std::vector<std::string_view> split_tabs(const std::string_view line) {
            std::vector<std::string_view> fields;
            std::size_t start = 0;
            while (true) {
                const auto tab = line.find('\t', start);
                fields.push_back(line.substr(start, tab == std::string_view::npos ? std::string_view::npos : tab - start));
                if (tab == std::string_view::npos) break;
                start = tab + 1;
            }
            return fields;
        }

        template <typename T>
        bool parse_number(const std::string_view text, T& value) {
            const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
            return ec == std::errc{} && ptr == text.data() + text.size();
        }

        std::tm local_time(const std::int64_t timestamp) {
            const auto t = static_cast<std::time_t>(timestamp);
            std::tm tm{};
#ifdef _WIN32
            localtime_s(&tm, &t);
#else
            localtime_r(&t, &tm);
#endif
            return tm;
        }

        void add(StatsSummary& summary, std::set<std::string>& runs, const StatRecord& r) {
            ++summary.files;
            summary.original_bytes += r.original_size;
            summary.saved_bytes += r.saved();
            runs.insert(r.run_id);
            summary.runs = runs.size();
            if (!summary.biggest || r.saved() > summary.biggest->saved()) summary.biggest = r;
        }
    } // namespace

    FileStatsStore::FileStatsStore(fs::path file) : file_(std::move(file)) {}

    std::vector<StatRecord> FileStatsStore::load() const {
        std::vector<StatRecord> records;
        std::ifstream in(file_, std::ios::binary);
        std::string line;
        bool header = true;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (header) {
                header = false;
                if (line == kHeader) continue;
            }
            const auto f = split_tabs(line);
            if (f.size() != 7) continue;
            StatRecord r;
            if (!parse_number(f[0], r.timestamp) || !parse_number(f[2], r.original_size) ||
                !parse_number(f[3], r.compressed_size) || !parse_number(f[4], r.duration_ms)) continue;
            r.extension = unescape(f[1]);
            r.run_id = unescape(f[5]);
            r.file_name = unescape(f[6]);
            records.push_back(std::move(r));
        }
        return records;
    }

    bool FileStatsStore::append(const std::vector<StatRecord>& records) {
        if (records.empty()) return true;
        std::error_code ec;
        if (file_.has_parent_path()) fs::create_directories(file_.parent_path(), ec);
        const bool fresh = !fs::exists(file_, ec) || fs::file_size(file_, ec) == 0;
        std::ofstream out(file_, std::ios::binary | std::ios::app);
        if (!out) return false;
        if (fresh) out << kHeader << '\n';
        for (const auto& r : records) {
            out << r.timestamp << '\t' << escape(r.extension) << '\t' << r.original_size << '\t'
                << r.compressed_size << '\t' << r.duration_ms << '\t' << escape(r.run_id) << '\t'
                << escape(r.file_name) << '\n';
        }
        out.close();
        return !out.fail();
    }

    bool FileStatsStore::clear() {
        std::error_code ec;
        fs::remove(file_, ec);
        return !ec;
    }

    StatsSummary summarize(const std::vector<StatRecord>& records) {
        StatsSummary summary;
        std::set<std::string> runs;
        for (const auto& r : records) add(summary, runs, r);
        return summary;
    }

    std::vector<PeriodStats> by_period(const std::vector<StatRecord>& records, const Period period) {
        std::map<std::string, std::pair<StatsSummary, std::set<std::string>>> groups;
        for (const auto& r : records) {
            const auto tm = local_time(r.timestamp);
            char key[16];
            if (period == Period::Day) {
                std::snprintf(key, sizeof key, "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
            } else {
                std::snprintf(key, sizeof key, "%04d-%02d", tm.tm_year + 1900, tm.tm_mon + 1);
            }
            auto& [summary, runs] = groups[key];
            add(summary, runs, r);
        }
        std::vector<PeriodStats> result;
        result.reserve(groups.size());
        for (auto& [key, group] : groups) result.push_back({key, std::move(group.first)});
        return result;
    }

} // namespace chisel_app
