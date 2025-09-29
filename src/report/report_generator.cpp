//
// Created by Giuseppe Francione on 20/09/25.
//

#include "report_generator.hpp"
#include <iostream>
#include <format>
#include <algorithm>
#include <fstream>

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

static unsigned get_terminal_width() {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi))
        return csbi.srWindow.Right - csbi.srWindow.Left + 1;
    return 80;
#else
    winsize w;
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

void print_console_report(const std::vector<Result>& results,
                          const unsigned num_threads,
                          double total_seconds) {
    const unsigned term_width = get_terminal_width();
    const bool use_colors = is_stdout_a_tty();

    // calculate column widths
    size_t max_mime = 15;
    size_t max_before = 12;
    size_t max_after = 12;
    size_t max_delta = 10;
    size_t max_time = 10;
    size_t max_result = 10;

    for (const auto& r : results) {
        max_mime   = std::max(max_mime,   strip_ansi(r.mime).size());
        max_before = std::max(max_before, std::to_string(r.size_before / 1024).size());
        max_after  = std::max(max_after,  std::to_string(r.size_after / 1024).size());
        double pct = r.success && r.size_before
                         ? 100.0 * (1.0 - static_cast<double>(r.size_after) / r.size_before)
                         : 0.0;
        const std::string delta = r.success ? std::format("{:.2f}%", pct) : "-";
        max_delta  = std::max(max_delta, strip_ansi(delta).size());
        max_time   = std::max(max_time,  std::format("{:.2f}", r.seconds).size());
        std::string outcome;
        if (!r.success) {
            outcome = use_colors ? "\033[1;31mFAIL\033[0m" : "FAIL";
        } else if (r.replaced) {
            outcome = use_colors ? "\033[1;32mOK (replaced)\033[0m" : "OK (replaced)";
        } else {
            outcome = use_colors ? "\033[1;33mOK (skipped)\033[0m" : "OK (skipped)";
        }
        max_result  = std::max(max_result, strip_ansi(outcome).size());
    }

    unsigned fixed_cols_width = max_mime + max_before + max_after +
                                 max_delta + max_time + max_result;

    fixed_cols_width += 6;

    const unsigned file_col_width = term_width > fixed_cols_width + 10
                                ? term_width - fixed_cols_width
                                : 20;

    auto truncate = [](const std::string& s, const size_t max_len) {
        return s.size() <= max_len ? s : s.substr(0, max_len - 3) + "...";
    };

    // header
    auto fmt_str = std::string("{:<") + std::to_string(file_col_width) + "}"
             + "{:<" + std::to_string(max_mime) + "}"
             + "{:<" + std::to_string(max_before) + "}"
             + "{:<" + std::to_string(max_after) + "}"
             + "{:<" + std::to_string(max_delta) + "}"
             + "{:<" + std::to_string(max_time) + "}"
             + "{:<" + std::to_string(max_result) + "}"
             + "{}\n";

std::cout << "\n" << std::vformat(fmt_str,
    std::make_format_args(
        "File", "MIME type", "Before(KB)", "After(KB)",
        "Delta(%)", "Time(s)", "Result", "" ));

    uintmax_t total_saved = 0;
    auto sorted = results;
    std::ranges::sort(sorted, [](const auto& a, const auto& b) { return a.filename < b.filename; });

    // row formatting
    auto row_fmt = std::string("{:<") + std::to_string(file_col_width) + "}"
                 + "{:<" + std::to_string(max_mime) + "}"
                 + "{:<" + std::to_string(max_before) + "}"
                 + "{:<" + std::to_string(max_after) + "}"
                 + "{:<" + std::to_string(max_delta) + "}"
                 + "{:<" + std::to_string(max_time) + "}"
                 + "{:<" + std::to_string(max_result) + "}"
                 + "{}\n";

    for (const auto& r : sorted) {
        double pct = r.success && r.size_before
                         ? 100.0 * (1.0 - static_cast<double>(r.size_after) / r.size_before)
                         : 0.0;
        std::string delta = r.success ? std::format("{:.2f}%", pct) : "-";
        std::string outcome = !r.success ? "\033[1;31mFAIL\033[0m"
                                         : r.replaced ? "\033[1;32mOK (replaced)\033[0m"
                                                      : "\033[1;33mOK (skipped)\033[0m";
        if (r.replaced && r.size_before > r.size_after)
            total_saved += r.size_before - r.size_after;

        auto filenamecolwidth = truncate(r.filename, file_col_width);
        auto sizeBefore = r.size_before / 1024;
        auto sizeAfter = r.size_after / 1024;
        std::cout << std::vformat(row_fmt,
           std::make_format_args(
               filenamecolwidth,
               r.mime,
               sizeBefore,
               sizeAfter,
               delta,
               r.seconds,
               outcome,
               "" ));
    }


    std::cout << "\nTotal saved space: " << (total_saved / 1024) << " KB\n";
    std::cout << "Total time: " << std::format("{:.2f}", total_seconds)
              << " s (" << num_threads << " thread"<< (num_threads > 1U ? "s" : "") << ")\n";
}

void export_csv_report(const std::vector<Result>& results,
                       const std::filesystem::path& output_path) {
    std::ofstream out(output_path);
    if (!out) return;

    out << "File,MIME,Before(KB),After(KB),Delta(%),Time(s),Result\n";
    for (const auto& r : results) {
        const double pct = r.success && r.size_before
                         ? 100.0 * (1.0 - static_cast<double>(r.size_after) / r.size_before)
                         : 0.0;
        const std::string outcome = !r.success ? "FAIL"
                                         : r.replaced ? "OK (replaced)"
                                                      : "OK (skipped)";
        out << '"' << r.filename << "\","
            << r.mime << ","
            << (r.size_before / 1024) << ","
            << (r.size_after / 1024) << ","
            << pct << ","
            << r.seconds << ","
            << outcome << "\n";
    }
}