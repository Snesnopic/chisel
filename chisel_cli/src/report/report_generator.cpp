//
// Created by Giuseppe Francione on 20/09/25.
//

#include "report_generator.hpp"
#include <iostream>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <iomanip>
#include "../cli/cli_parser.hpp"

#ifdef _WIN32

#include <windows.h>
#include <io.h>      // _isatty, _fileno
#define isatty _isatty
#define fileno _fileno

#else

#include <sys/ioctl.h>
#include <unistd.h>

#endif

static bool is_stdout_a_tty() {
    return isatty(fileno(stdout)) != 0;
}

unsigned get_terminal_width() {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi))
        return csbi.srWindow.Right - csbi.srWindow.Left + 1;
    return 80;
#else
    winsize w{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0)
        return w.ws_col;
    return 80;
#endif
}

#include <regex>

static std::string strip_ansi(const std::string& s) {
    static const std::regex ansi_pattern("\033\\[[0-9;]*m");
    return std::regex_replace(s, ansi_pattern, "");
}

static std::string csv_escape(const std::string& data) {
    if (data.find_first_of(",\"\n\r") == std::string::npos) {
        return data;
    }
    std::string result;
    result.reserve(data.size() + 4);
    result.push_back('"');
    for (char c : data) {
        if (c == '"') {
            result.push_back('"'); // escape quote with another quote
        }
        result.push_back(c);
    }
    result.push_back('"');
    return result;
}

void print_console_report(const std::vector<Result>& results,
                          const std::vector<ContainerResult>& container_results,
                          const unsigned num_threads,
                          double total_seconds,
                          EncodeMode mode) {
    const unsigned term_width = get_terminal_width();
    const bool use_colors = is_stdout_a_tty();

    size_t max_mime = 15;
    size_t max_before = 12;
    size_t max_after = 12;
    size_t max_delta = 10;
    size_t max_time = 10;
    size_t max_result = 10;
    size_t max_error = 5;
    #ifdef max
    #undef max
    #endif
    for (const auto& r : results) {
        max_mime   = std::max(max_mime,   strip_ansi(r.mime).size());
        max_before = std::max(max_before, std::to_string(r.size_before / 1024).size());
        max_after  = std::max(max_after,  std::to_string(r.size_after / 1024).size());
        double pct = r.success && r.size_before
                         ? 100.0 * (1.0 - static_cast<double>(r.size_after) / static_cast<double>(r.size_before))
                         : 0.0;
        std::ostringstream ossd;
        ossd << std::fixed << std::setprecision(2) << pct << "%";
        const std::string delta = r.success ? ossd.str() : "-";
        std::ostringstream osst;
        osst << std::fixed << std::setprecision(2) << r.seconds;
        max_delta  = std::max(max_delta, strip_ansi(delta).size());
        max_time   = std::max(max_time,  osst.str().size());
        std::string outcome;
        if (!r.success) {
            outcome = use_colors ? "\033[1;31mFAIL\033[0m" : "FAIL";
        } else if (r.replaced) {
            outcome = use_colors ? "\033[1;32mOK (replaced)\033[0m" : "OK (replaced)";
        } else {
            outcome = use_colors ? "\033[1;33mOK (skipped)\033[0m" : "OK (skipped)";
        }
        max_result  = std::max(max_result, strip_ansi(outcome).size());
        max_error   = std::max(max_error, strip_ansi(r.error_msg).size());
    }

    unsigned fixed_cols_width = max_mime + max_before + max_after +
                                 max_delta + max_time + max_result + max_error;

    fixed_cols_width += 7;

    const unsigned file_col_width = term_width > fixed_cols_width + 5
                                ? term_width - fixed_cols_width
                                : 10;

    auto truncate = [](const std::string& s, const size_t max_len) {
        return s.size() <= max_len ? s : s.substr(0, max_len - 3) + "...";
    };

    std::cerr << "\n"
              << std::left << std::setw(file_col_width) << "File"
              << std::setw(max_mime)   << "MIME type"
              << std::setw(max_before)<< "Before(KB)"
              << std::setw(max_after) << "After(KB)"
              << std::setw(max_delta) << "Delta(%)"
              << std::setw(max_time)  << "Time(s)"
              << std::setw(max_result)<< "Result"
              << std::setw(max_error) << "Error"
              << "\n";
    uintmax_t total_original = 0;
    uintmax_t total_saved = 0;
    auto sorted = results;
    std::ranges::sort(sorted, [](const auto& a, const auto& b) {
        return a.path < b.path;
    });

    for (const auto& r : sorted) {
        double pct = r.success && r.size_before
                         ? 100.0 * (1.0 - static_cast<double>(r.size_after) / static_cast<double>(r.size_before))
                         : 0.0;
        std::ostringstream ossd;
        ossd << std::fixed << std::setprecision(2) << pct << "%";
        std::string delta = r.success ? ossd.str() : "-";
        std::string outcome = !r.success ? "\033[1;31mFAIL\033[0m"
                                         : r.replaced ? "\033[1;32mOK (replaced)\033[0m"
                                                      : "\033[1;33mOK (skipped)\033[0m";
        total_original += r.size_before;
        if (r.replaced && r.size_before > r.size_after)
            total_saved += r.size_before - r.size_after;

        auto filenamecolwidth = truncate(r.path.filename().string(), file_col_width);
        if (r.container_origin) {
            filenamecolwidth = "  ↳ " + filenamecolwidth;
        }

        auto sizeBefore = r.size_before / 1024;
        auto sizeAfter = r.size_after / 1024;
        std::ostringstream osst;
        osst << std::fixed << std::setprecision(2) << r.seconds;
        std::cerr << std::left << std::setw(file_col_width) << filenamecolwidth
                  << std::setw(max_mime)   << r.mime
                  << std::setw(max_before)<< sizeBefore
                  << std::setw(max_after) << sizeAfter
                  << std::setw(max_delta) << delta
                  << std::setw(max_time)  << osst.str()
                  << std::setw(max_result)<< outcome
                  << std::setw(max_error) << r.error_msg
                  << "\n";

        if (!r.codecs_used.empty()) {
            if (mode == EncodeMode::PIPE) {
                std::cerr << "    Pipeline: ";
                for (size_t i = 0; i < r.codecs_used.size(); ++i) {
                    std::ostringstream ossc;
                    ossc << std::fixed << std::setprecision(2) << r.codecs_used[i].second;
                    std::cerr << r.codecs_used[i].first << " (" << ossc.str() << "%)";
                    if (i + 1 < r.codecs_used.size()) std::cerr << " -> ";
                }
                std::cerr << "\n";
            } else {
                std::cerr << "    Tried: ";
                for (size_t i = 0; i < r.codecs_used.size(); ++i) {
                    std::ostringstream ossc;
                    ossc << std::fixed << std::setprecision(2) << r.codecs_used[i].second;
                    std::cerr << r.codecs_used[i].first << " (" << ossc.str() << "%)";
                    if (i + 1 < r.codecs_used.size()) std::cerr << "; ";
                }
                std::cerr << "\n";
            }
        }
    }

    if (!container_results.empty()) {
        std::cerr << "\n=== Container results ===\n";
        std::cerr << std::left << std::setw(40) << "Container"
                  << std::setw(12) << "Format"
                  << std::setw(12) << "Before(KB)"
                  << std::setw(12) << "After(KB)"
                  << std::setw(8)  << "Delta(%)"
                  << "Error"
                  << "\n";

        for (const auto& c : container_results) {
            double pct = c.success && c.size_before
                         ? 100.0 * (1.0 - static_cast<double>(c.size_after) / static_cast<double>(c.size_before))
                         : 0.0;
            std::ostringstream ossd;
            ossd << std::fixed << std::setprecision(2) << pct << "%";
            std::string delta = c.success ? ossd.str() : "-";
            const auto size_before = c.size_before / 1024;
            const auto size_after = c.size_after / 1024;
            const auto fileName = c.filename.filename().string();
            std::cerr << std::left << std::setw(40) << fileName
                      << std::setw(12) << size_before
                      << std::setw(12) << size_after
                      << std::setw(8)  << delta
                      << c.error_msg
                      << "\n";
        }
    }

    std::cerr << "\nTotal saved space: " << (total_saved / 1024) << " KB\n";
    if (total_original > 0) {
        double total_pct = 100.0 * (static_cast<double>(total_saved) / static_cast<double>(total_original));
        std::cerr << "Total reduction: " << std::fixed << std::setprecision(2) << total_pct << "%\n";
    }
    std::cerr << "Total time: " << std::fixed << std::setprecision(2)
              << total_seconds << " s (" << num_threads << " thread"
              << (num_threads > 1U ? "s" : "") << ")\n";
}

void export_csv_report(const std::vector<Result>& results,
                       const std::vector<ContainerResult>& container_results,
                       const std::filesystem::path& output_path,
                       const double total_seconds,
                       const EncodeMode mode) {
    std::ofstream out(output_path);
    if (!out) return;

    out << "File,Container,MIME,Before(KB),After(KB),Delta(%),Time(s),Result,Codecs,Error\n";

    for (const auto& r : results) {
        const double pct = r.success && r.size_before
                         ? 100.0 * (1.0 - static_cast<double>(r.size_after) / static_cast<double>(r.size_before))
                         : 0.0;
        const std::string outcome = !r.success ? "FAIL"
                                         : r.replaced ? "OK (replaced)"
                                                      : "OK (skipped)";

        std::string codecs_str;
        for (size_t i = 0; i < r.codecs_used.size(); ++i) {
            std::ostringstream ossc;
            ossc << std::fixed << std::setprecision(2) << r.codecs_used[i].second;
            codecs_str += r.codecs_used[i].first + ":" + ossc.str() + "%";
            if (i + 1 < r.codecs_used.size()) {
                if (mode == EncodeMode::PIPE)
                    codecs_str += " -> ";
                else
                    codecs_str += "; ";
            }
        }

        out << csv_escape(r.path.filename().string()) << ","
            << csv_escape(r.container_origin ? r.container_origin->filename().string() : "") << ","
            << csv_escape(r.mime) << ","
            << (r.size_before / 1024) << ","
            << (r.size_after / 1024) << ",";

        std::ostringstream osspct;
        osspct << std::fixed << std::setprecision(2) << pct;
        out << osspct.str() << ",";

        std::ostringstream osstime;
        osstime << std::fixed << std::setprecision(2) << r.seconds;
        out << osstime.str() << ","
            << csv_escape(outcome) << ","
            << csv_escape(codecs_str) << ","
            << csv_escape(r.error_msg) << "\n";
    }
    if (!container_results.empty()) {
        out << "\n\nContainer,Format,Before(KB),After(KB),Delta(%),Error\n";
        for (const auto& c : container_results) {
            double pct = c.success && c.size_before
                         ? 100.0 * (1.0 - static_cast<double>(c.size_after) / static_cast<double>(c.size_before))
                         : 0.0;
            out << csv_escape(c.filename.filename().string()) << ","
                << csv_escape(c.format) << ","
                << (c.size_before / 1024) << ","
                << (c.size_after / 1024) << ",";
            std::ostringstream osspct;
            osspct << std::fixed << std::setprecision(2) << (c.success ? pct : 0.0);
            out << osspct.str() << ","
                << csv_escape(c.error_msg) << "\n";
        }
    }

    out << "\n\nTotal amount of time,Encoding mode used\n";
    out << std::fixed << std::setprecision(2) << total_seconds << " seconds,"
        << (mode == EncodeMode::PIPE ? "PIPE" : "PARALLEL") << "\n";
}