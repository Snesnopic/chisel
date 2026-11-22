#ifndef CHISEL_WIN_STATS_WINDOW_HPP
#define CHISEL_WIN_STATS_WINDOW_HPP

#include "chisel_app/stats.hpp"

#include <windows.h>

#include <string>
#include <vector>

namespace chisel_win {

    // modeless window with totals, space saved by day or month and by format
    class StatsWindow {
    public:
        StatsWindow(HINSTANCE instance, chisel_app::StatsStore& stats);
        ~StatsWindow();
        StatsWindow(const StatsWindow&) = delete;
        StatsWindow& operator=(const StatsWindow&) = delete;

        void show(HWND owner);
        void refresh();
        void theme_changed();
        [[nodiscard]] HWND hwnd() const { return hwnd_; }

    private:
        struct Bar {
            std::uint64_t saved = 0;
            std::wstring label;
        };
        struct Format {
            std::string extension;
            std::uint64_t saved = 0;
        };

        static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
        LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
        void layout();
        void paint(HDC dc);
        void export_csv();
        void reset();
        [[nodiscard]] int s(int px) const;

        HINSTANCE instance_;
        chisel_app::StatsStore& stats_;
        HWND hwnd_ = nullptr;
        HWND export_button_ = nullptr;
        HWND reset_button_ = nullptr;
        UINT dpi_ = 96;
        bool by_month_ = false;
        RECT seg_day_{}, seg_month_{};

        std::vector<chisel_app::StatRecord> records_;
        chisel_app::StatsSummary summary_;
        double average_percent_ = 0;
        std::wstring since_;
        std::vector<Bar> days_;
        std::vector<Bar> months_;
        std::vector<Format> formats_;
    };

} // namespace chisel_win

#endif // CHISEL_WIN_STATS_WINDOW_HPP
