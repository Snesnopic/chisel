#ifndef CHISEL_WIN_UI_HPP
#define CHISEL_WIN_UI_HPP

#include <windows.h>
#include <commctrl.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace chisel_win {

    enum Category { CatImage, CatAudio, CatVideo, CatArchive, CatDoc, CatApp, CatCount };

    // colors of the approved design, light and dark
    struct Palette {
        bool dark = false;
        COLORREF window, surface, panel, child_bg, row_line, border, text, text2, text3, accent, on_accent, link,
            sel_bg, err_bg, err_row_bg, err_line, err, ok, warn, pending_dot, bar_track, bar_fill, log_bg, dash,
            ctrl_bg, ctrl_border, disabled_bg, disabled_fg, card, grid, axis, seg_on, hover, info_bg, info_line;
        COLORREF badge_bg[CatCount];
        COLORREF badge_fg[CatCount];
    };

    const Palette& palette();
    bool is_dark();
    void init_dark_mode_support();
    bool refresh_theme(); // true when light/dark changed
    void apply_window_theme(HWND hwnd);
    void apply_control_theme(HWND control);

    struct Fonts {
        HFONT ui = nullptr, ui_semibold = nullptr, ui_small = nullptr, ui_small_semibold = nullptr, title = nullptr,
              big = nullptr, big_bold = nullptr, heading = nullptr, name = nullptr, badge = nullptr,
              badge_big = nullptr, mono = nullptr, icons = nullptr, icons_small = nullptr, icons_big = nullptr;
    };
    const Fonts& fonts(UINT dpi);
    void reset_fonts();
    int scale(int px, UINT dpi);

    // segoe fluent icons on windows 11, segoe mdl2 assets on windows 10
    namespace glyph {
        constexpr wchar_t add_file = 0xE8E5, add_folder = 0xE8F4, play = 0xE768, stop = 0xE71A, trash = 0xE74D,
                          chart = 0xE9D2, settings = 0xE713, info = 0xE946, chevron_down = 0xE70D,
                          chevron_right = 0xE76C, folder_open = 0xE838, copy = 0xE8C8, check = 0xE73E,
                          error = 0xE783, upload = 0xE898, download = 0xE896;
    }

    void fill_rect(HDC dc, const RECT& rc, COLORREF color);
    void fill_round_rect(HDC dc, const RECT& rc, int radius, COLORREF fill, COLORREF border = CLR_NONE);
    void frame_rect(HDC dc, const RECT& rc, COLORREF color);
    void draw_text(HDC dc, std::wstring_view text, RECT rc, COLORREF color, HFONT font, UINT format);
    void draw_glyph(HDC dc, wchar_t g, const RECT& rc, COLORREF color, HFONT font);
    SIZE measure_text(HDC dc, std::wstring_view text, HFONT font);
    void draw_dot(HDC dc, int cx, int cy, int diameter, COLORREF color);

    // owner-drawn buttons in the design's three styles
    enum class ButtonStyle { Normal, Accent, Link };
    void draw_button(const DRAWITEMSTRUCT& di, ButtonStyle style, wchar_t icon, UINT dpi);

    std::wstring widen(std::string_view utf8);
    std::string narrow(std::wstring_view text);
    std::wstring format_bytes(std::uint64_t bytes);
    std::wstring format_duration(std::chrono::milliseconds ms);
    std::wstring format_percent(double percent);
    Category category_for(const std::string& extension);
    std::wstring badge_text(const std::string& extension);

    std::wstring window_text(HWND hwnd);
    bool copy_to_clipboard(HWND owner, const std::wstring& text);

} // namespace chisel_win

#endif // CHISEL_WIN_UI_HPP
