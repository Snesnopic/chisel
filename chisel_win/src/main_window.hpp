#ifndef CHISEL_WIN_MAIN_WINDOW_HPP
#define CHISEL_WIN_MAIN_WINDOW_HPP

#include "chisel_app/session.hpp"
#include "chisel_app/settings.hpp"
#include "chisel_app/stats.hpp"

#include <windows.h>
#include <commctrl.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

struct ITaskbarList3;

namespace chisel_win {

    class StatsWindow;

    class MainWindow {
    public:
        MainWindow(HINSTANCE instance, chisel_app::Session& session, chisel_app::KeyValueStore& store,
                   chisel_app::StatsStore& stats);
        ~MainWindow();
        MainWindow(const MainWindow&) = delete;
        MainWindow& operator=(const MainWindow&) = delete;

        bool create(int show);
        [[nodiscard]] HWND hwnd() const { return hwnd_; }
        bool pre_translate(MSG& msg);
        void add_paths(std::vector<std::filesystem::path> paths);

    private:
        struct ToolItem {
            enum Kind { Button, Accent, Separator, Spacer } kind = Button;
            int command = 0;
            wchar_t glyph = 0;
            std::wstring text{};
            bool split = false;
            bool chevron = false;
            bool icon_only = false;
            bool right = false;
            bool enabled = true;
            RECT rc{};
            RECT drop{};
        };
        enum class InfoKind { None, Errors, Notice };

        static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
        // the header's notifications go to the list, not to this window
        static LRESULT CALLBACK list_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR self);
        LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);

        void create_children();
        void apply_theme();
        void apply_fonts();
        void layout();
        void layout_toolbar(int width);
        void paint(HDC dc);
        void paint_toolbar(HDC dc);
        void paint_info(HDC dc);
        void paint_inspector(HDC dc);
        void paint_status(HDC dc);
        void paint_empty(HDC dc);
        LRESULT on_notify(NMHDR* hdr);
        LRESULT draw_list_row(NMLVCUSTOMDRAW* cd);
        LRESULT draw_header_item(NMCUSTOMDRAW* cd);
        void on_command(int id);
        void on_changed();
        void on_run_finished();
        void rebuild_visible();
        void restore_selection();
        void select_row(std::uint64_t id, bool ensure_visible);
        void toggle(std::uint64_t id);
        void update_toolbar_state();
        void update_inspector();
        void update_taskbar();
        void show_menu(const ToolItem& item, bool start_menu);
        void start(bool dry_run);
        void open_settings();
        void open_stats();
        void show_info(InfoKind kind, std::wstring title, std::wstring text, bool action);
        void clear_info();
        void write_log_file();
        [[nodiscard]] const chisel_app::Row* selected_row() const;
        [[nodiscard]] const chisel_app::Row* row_by_id(std::uint64_t id) const;
        [[nodiscard]] int hit_tool(POINT pt, bool* in_drop) const;
        [[nodiscard]] RECT chevron_rect(int index) const;
        [[nodiscard]] COLORREF region_color(HWND control) const;
        [[nodiscard]] int s(int px) const;

        HINSTANCE instance_;
        chisel_app::Session& session_;
        chisel_app::KeyValueStore& store_;
        chisel_app::StatsStore& stats_;
        chisel_app::Settings settings_;

        HWND hwnd_ = nullptr;
        HWND list_ = nullptr;
        HWND log_ = nullptr;
        HWND open_button_ = nullptr;
        HWND copy_button_ = nullptr;
        HWND empty_files_ = nullptr;
        HWND empty_folder_ = nullptr;
        HWND formats_link_ = nullptr;
        HWND info_button_ = nullptr;
        HIMAGELIST row_height_ = nullptr;
        HBRUSH panel_brush_ = nullptr;
        HBRUSH log_brush_ = nullptr;
        HACCEL accel_ = nullptr;
        UINT dpi_ = 96;

        chisel_app::Snapshot snap_;
        std::vector<std::size_t> visible_;
        std::unordered_set<std::uint64_t> collapsed_;
        std::uint64_t selected_id_ = 0;
        bool selecting_ = false;
        bool was_running_ = false;
        bool close_after_stop_ = false;
        std::wstring log_text_;
        std::uint64_t log_seq_ = 0;

        std::vector<ToolItem> tools_;
        int hot_tool_ = -1;
        int pressed_tool_ = -1;
        bool tracking_mouse_ = false;

        RECT toolbar_rc_{}, info_rc_{}, list_rc_{}, inspector_rc_{}, status_rc_{}, content_rc_{};
        InfoKind info_kind_ = InfoKind::None;
        std::wstring info_title_, info_text_;
        bool info_action_ = false;
        std::uint64_t info_target_ = 0;

        std::mutex add_mutex_;
        std::vector<std::vector<std::filesystem::path>> add_queue_;
        std::atomic<bool> adding_{false};
        std::thread add_thread_;

        std::unique_ptr<StatsWindow> stats_window_;
        ITaskbarList3* taskbar_ = nullptr;
        UINT taskbar_created_ = 0;
    };

} // namespace chisel_win

#endif // CHISEL_WIN_MAIN_WINDOW_HPP
