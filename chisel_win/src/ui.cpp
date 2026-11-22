#include "ui.hpp"

#include "chisel_app/formats.hpp"

#include <commctrl.h>
#include <dwmapi.h>
#include <shlwapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <cctype>
#include <cwchar>
#include <map>
#include <unordered_set>

namespace chisel_win {

    namespace {
        // windows 10 1809: the uxtheme ordinals below and DWM's dark title bar (attribute 19) appear
        constexpr DWORD kDarkModeBuild = 17763;
        // windows 10 2004: the dark title bar attribute becomes 20
        constexpr DWORD kDarkAttribute20Build = 19041;

        // the real build number: GetVersionEx reports what the manifest declares
        DWORD windows_build() {
            using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
            static const DWORD build = [] {
                RTL_OSVERSIONINFOW info{};
                info.dwOSVersionInfoSize = sizeof(info);
                const auto get = reinterpret_cast<RtlGetVersionFn>(
                    reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")));
                return get && get(&info) == 0 ? info.dwBuildNumber : DWORD{0};
            }();
            return build;
        }

        bool dark_mode_available() { return windows_build() >= kDarkModeBuild; }

        enum class PreferredAppMode { Default, AllowDark, ForceDark, ForceLight, Max };
        using SetPreferredAppModeFn = PreferredAppMode(WINAPI*)(PreferredAppMode);
        using AllowDarkModeForWindowFn = BOOL(WINAPI*)(HWND, BOOL);
        using FlushMenuThemesFn = void(WINAPI*)();

        AllowDarkModeForWindowFn g_allow_dark = nullptr;
        FlushMenuThemesFn g_flush_menus = nullptr;

        Palette make_light() {
            Palette p{};
            p.dark = false;
            p.window = RGB(0xF3, 0xF3, 0xF3); p.surface = RGB(0xFF, 0xFF, 0xFF); p.panel = RGB(0xFA, 0xFA, 0xFA);
            p.child_bg = RGB(0xF6, 0xF6, 0xF6); p.row_line = RGB(0xF0, 0xF0, 0xF0); p.border = RGB(0xE3, 0xE3, 0xE3);
            p.text = RGB(0x1B, 0x1B, 0x1B); p.text2 = RGB(0x5A, 0x5A, 0x5A); p.text3 = RGB(0x66, 0x66, 0x66);
            p.accent = RGB(0x00, 0x5F, 0xB8); p.on_accent = RGB(0xFF, 0xFF, 0xFF); p.link = RGB(0x00, 0x5F, 0xB8);
            p.sel_bg = RGB(0xE0, 0xEB, 0xF7); p.err_bg = RGB(0xFD, 0xEC, 0xEC); p.err_row_bg = RGB(0xFD, 0xF0, 0xF0);
            p.err_line = RGB(0xF4, 0xC7, 0xC7); p.err = RGB(0xB4, 0x23, 0x18); p.ok = RGB(0x10, 0x7C, 0x10);
            p.warn = RGB(0x8A, 0x53, 0x00); p.pending_dot = RGB(0xBD, 0xBD, 0xBD); p.bar_track = RGB(0xE5, 0xE5, 0xE5);
            p.bar_fill = RGB(0x3A, 0x9D, 0x3A); p.log_bg = RGB(0xFF, 0xFF, 0xFF); p.dash = RGB(0xBD, 0xBD, 0xBD);
            p.ctrl_bg = RGB(0xFF, 0xFF, 0xFF); p.ctrl_border = RGB(0xD1, 0xD1, 0xD1); p.disabled_bg = RGB(0xE6, 0xE6, 0xE6);
            p.disabled_fg = RGB(0x8F, 0x8F, 0x8F); p.card = RGB(0xFF, 0xFF, 0xFF); p.grid = RGB(0xEB, 0xEB, 0xEB);
            p.axis = RGB(0xD1, 0xD1, 0xD1); p.seg_on = RGB(0xE0, 0xEB, 0xF7); p.hover = RGB(0xE6, 0xE6, 0xE6);
            p.info_bg = RGB(0xE1, 0xED, 0xFA); p.info_line = RGB(0xB8, 0xD3, 0xF0);
            const COLORREF bg[CatCount] = {RGB(0xDD, 0xF0, 0xE1), RGB(0xED, 0xE4, 0xF7), RGB(0xFC, 0xE4, 0xEC),
                                           RGB(0xFF, 0xF1, 0xD6), RGB(0xE1, 0xED, 0xFA), RGB(0xEC, 0xEC, 0xEC)};
            const COLORREF fg[CatCount] = {RGB(0x17, 0x60, 0x2B), RGB(0x5B, 0x2C, 0x91), RGB(0x9A, 0x1B, 0x4A),
                                           RGB(0x7A, 0x4B, 0x00), RGB(0x0B, 0x4F, 0x94), RGB(0x42, 0x42, 0x42)};
            std::copy(std::begin(bg), std::end(bg), p.badge_bg);
            std::copy(std::begin(fg), std::end(fg), p.badge_fg);
            return p;
        }

        Palette make_dark() {
            Palette p{};
            p.dark = true;
            p.window = RGB(0x20, 0x20, 0x20); p.surface = RGB(0x27, 0x27, 0x27); p.panel = RGB(0x2B, 0x2B, 0x2B);
            p.child_bg = RGB(0x22, 0x22, 0x22); p.row_line = RGB(0x2E, 0x2E, 0x2E); p.border = RGB(0x3A, 0x3A, 0x3A);
            p.text = RGB(0xFF, 0xFF, 0xFF); p.text2 = RGB(0xC5, 0xC5, 0xC5); p.text3 = RGB(0xAD, 0xAD, 0xAD);
            p.accent = RGB(0x60, 0xCD, 0xFF); p.on_accent = RGB(0x00, 0x1A, 0x2C); p.link = RGB(0x60, 0xCD, 0xFF);
            p.sel_bg = RGB(0x37, 0x40, 0x4A); p.err_bg = RGB(0x3E, 0x25, 0x26); p.err_row_bg = RGB(0x3A, 0x23, 0x24);
            p.err_line = RGB(0x6B, 0x35, 0x37); p.err = RGB(0xFF, 0x99, 0xA4); p.ok = RGB(0x6C, 0xCB, 0x5F);
            p.warn = RGB(0xFC, 0xE1, 0x00); p.pending_dot = RGB(0x5A, 0x5A, 0x5A); p.bar_track = RGB(0x3D, 0x3D, 0x3D);
            p.bar_fill = RGB(0x6C, 0xCB, 0x5F); p.log_bg = RGB(0x1F, 0x1F, 0x1F); p.dash = RGB(0x5A, 0x5A, 0x5A);
            p.ctrl_bg = RGB(0x2D, 0x2D, 0x2D); p.ctrl_border = RGB(0x4A, 0x4A, 0x4A); p.disabled_bg = RGB(0x3A, 0x3A, 0x3A);
            p.disabled_fg = RGB(0x8A, 0x8A, 0x8A); p.card = RGB(0x2B, 0x2B, 0x2B); p.grid = RGB(0x35, 0x35, 0x35);
            p.axis = RGB(0x4A, 0x4A, 0x4A); p.seg_on = RGB(0x45, 0x45, 0x45); p.hover = RGB(0x33, 0x33, 0x33);
            p.info_bg = RGB(0x1E, 0x32, 0x47); p.info_line = RGB(0x2F, 0x4E, 0x6E);
            const COLORREF bg[CatCount] = {RGB(0x1F, 0x3A, 0x26), RGB(0x35, 0x26, 0x46), RGB(0x46, 0x22, 0x2F),
                                           RGB(0x43, 0x36, 0x1C), RGB(0x1E, 0x32, 0x47), RGB(0x36, 0x36, 0x36)};
            const COLORREF fg[CatCount] = {RGB(0x9B, 0xE0, 0xAA), RGB(0xD3, 0xB8, 0xF5), RGB(0xF7, 0xA9, 0xC4),
                                           RGB(0xF5, 0xD0, 0x8A), RGB(0xA9, 0xCD, 0xF4), RGB(0xD6, 0xD6, 0xD6)};
            std::copy(std::begin(bg), std::end(bg), p.badge_bg);
            std::copy(std::begin(fg), std::end(fg), p.badge_fg);
            return p;
        }

        Palette g_palette = make_light();

        bool system_uses_dark() {
            // before 1809 a dark window would keep a light title bar, light menus and light scroll bars
            if (!dark_mode_available()) return false;
            DWORD value = 1;
            DWORD size = sizeof(value);
            if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                             L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS) {
                return false;
            }
            return value == 0;
        }

        int CALLBACK font_exists_proc(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM found) {
            *reinterpret_cast<bool*>(found) = true;
            return 0;
        }

        bool font_installed(const wchar_t* face) {
            LOGFONTW lf{};
            lf.lfCharSet = DEFAULT_CHARSET;
            wcsncpy_s(lf.lfFaceName, face, _TRUNCATE);
            bool found = false;
            HDC dc = GetDC(nullptr);
            EnumFontFamiliesExW(dc, &lf, font_exists_proc, reinterpret_cast<LPARAM>(&found), 0);
            ReleaseDC(nullptr, dc);
            return found;
        }

        const wchar_t* icon_face() {
            static const wchar_t* face = font_installed(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
            return face;
        }

        std::map<UINT, Fonts> g_fonts;

        HFONT make_font(const wchar_t* face, int px, int weight, UINT dpi) {
            LOGFONTW lf{};
            lf.lfHeight = -MulDiv(px, static_cast<int>(dpi), 96);
            lf.lfWeight = weight;
            lf.lfCharSet = DEFAULT_CHARSET;
            lf.lfQuality = CLEARTYPE_QUALITY;
            wcsncpy_s(lf.lfFaceName, face, _TRUNCATE);
            return CreateFontIndirectW(&lf);
        }
    } // namespace

    const Palette& palette() { return g_palette; }
    bool is_dark() { return g_palette.dark; }

    void init_dark_mode_support() {
        // undocumented uxtheme ordinals: before 1809 the same numbers may be other functions, or missing.
        // 135 is AllowDarkModeForApp(BOOL) on 1809 and SetPreferredAppMode from 1903: AllowDark (1) suits both
        HMODULE ux = dark_mode_available() ? LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32) : nullptr;
        if (ux) {
            auto set_mode = reinterpret_cast<SetPreferredAppModeFn>(reinterpret_cast<void*>(GetProcAddress(ux, MAKEINTRESOURCEA(135))));
            g_allow_dark = reinterpret_cast<AllowDarkModeForWindowFn>(reinterpret_cast<void*>(GetProcAddress(ux, MAKEINTRESOURCEA(133))));
            g_flush_menus = reinterpret_cast<FlushMenuThemesFn>(reinterpret_cast<void*>(GetProcAddress(ux, MAKEINTRESOURCEA(136))));
            if (set_mode) set_mode(PreferredAppMode::AllowDark);
            if (g_flush_menus) g_flush_menus();
        }
        g_palette = system_uses_dark() ? make_dark() : make_light();
    }

    bool refresh_theme() {
        const bool dark = system_uses_dark();
        if (dark == g_palette.dark) return false;
        g_palette = dark ? make_dark() : make_light();
        if (g_flush_menus) g_flush_menus();
        return true;
    }

    void apply_window_theme(HWND hwnd) {
        if (!dark_mode_available()) return;
        BOOL dark = g_palette.dark ? TRUE : FALSE;
        const DWORD attribute = windows_build() >= kDarkAttribute20Build ? 20 : 19;
        DwmSetWindowAttribute(hwnd, attribute, &dark, sizeof(dark));
        if (g_allow_dark) g_allow_dark(hwnd, dark);
    }

    void apply_control_theme(HWND control) {
        if (g_allow_dark) g_allow_dark(control, g_palette.dark ? TRUE : FALSE);
        SetWindowTheme(control, g_palette.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        SendMessageW(control, WM_THEMECHANGED, 0, 0);
    }

    int scale(const int px, const UINT dpi) { return MulDiv(px, static_cast<int>(dpi), 96); }

    const Fonts& fonts(const UINT dpi) {
        auto it = g_fonts.find(dpi);
        if (it != g_fonts.end()) return it->second;
        NONCLIENTMETRICSW ncm{};
        ncm.cbSize = sizeof(ncm);
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        const wchar_t* face = ncm.lfMessageFont.lfFaceName[0] ? ncm.lfMessageFont.lfFaceName : L"Segoe UI";
        Fonts f;
        f.ui = make_font(face, 13, FW_NORMAL, dpi);
        f.ui_semibold = make_font(face, 13, FW_SEMIBOLD, dpi);
        f.ui_small = make_font(face, 12, FW_NORMAL, dpi);
        f.ui_small_semibold = make_font(face, 12, FW_SEMIBOLD, dpi);
        f.heading = make_font(face, 14, FW_SEMIBOLD, dpi);
        f.name = make_font(face, 15, FW_SEMIBOLD, dpi);
        f.title = make_font(face, 20, FW_SEMIBOLD, dpi);
        f.big = make_font(face, 28, FW_SEMIBOLD, dpi);
        f.big_bold = make_font(face, 28, FW_BOLD, dpi);
        f.badge = make_font(face, 10, FW_BOLD, dpi);
        f.badge_big = make_font(face, 12, FW_BOLD, dpi);
        f.mono = make_font(font_installed(L"Cascadia Mono") ? L"Cascadia Mono" : L"Consolas", 12, FW_NORMAL, dpi);
        f.icons = make_font(icon_face(), 16, FW_NORMAL, dpi);
        f.icons_small = make_font(icon_face(), 12, FW_NORMAL, dpi);
        f.icons_big = make_font(icon_face(), 48, FW_NORMAL, dpi);
        return g_fonts.emplace(dpi, f).first->second;
    }

    void reset_fonts() {
        for (auto& [dpi, f] : g_fonts) {
            for (HFONT h : {f.ui, f.ui_semibold, f.ui_small, f.ui_small_semibold, f.title, f.big, f.big_bold, f.heading, f.name,
                            f.badge, f.badge_big, f.mono, f.icons, f.icons_small, f.icons_big}) {
                if (h) DeleteObject(h);
            }
        }
        g_fonts.clear();
    }

    void fill_rect(HDC dc, const RECT& rc, const COLORREF color) {
        const COLORREF old = SetBkColor(dc, color);
        ExtTextOutW(dc, 0, 0, ETO_OPAQUE, &rc, nullptr, 0, nullptr);
        SetBkColor(dc, old);
    }

    void fill_round_rect(HDC dc, const RECT& rc, const int radius, const COLORREF fill, const COLORREF border) {
        HBRUSH brush = CreateSolidBrush(fill);
        HPEN pen = CreatePen(PS_SOLID, 1, border == CLR_NONE ? fill : border);
        auto old_brush = SelectObject(dc, brush);
        auto old_pen = SelectObject(dc, pen);
        RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, radius * 2, radius * 2);
        SelectObject(dc, old_brush);
        SelectObject(dc, old_pen);
        DeleteObject(brush);
        DeleteObject(pen);
    }

    void frame_rect(HDC dc, const RECT& rc, const COLORREF color) {
        HBRUSH brush = CreateSolidBrush(color);
        FrameRect(dc, &rc, brush);
        DeleteObject(brush);
    }

    void draw_text(HDC dc, const std::wstring_view text, RECT rc, const COLORREF color, HFONT font, const UINT format) {
        auto old_font = SelectObject(dc, font);
        const COLORREF old_color = SetTextColor(dc, color);
        const int old_mode = SetBkMode(dc, TRANSPARENT);
        DrawTextW(dc, text.data(), static_cast<int>(text.size()), &rc, format | DT_NOPREFIX);
        SetBkMode(dc, old_mode);
        SetTextColor(dc, old_color);
        SelectObject(dc, old_font);
    }

    void draw_glyph(HDC dc, const wchar_t g, const RECT& rc, const COLORREF color, HFONT font) {
        draw_text(dc, std::wstring_view(&g, 1), rc, color, font, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    SIZE measure_text(HDC dc, const std::wstring_view text, HFONT font) {
        auto old_font = SelectObject(dc, font);
        SIZE size{};
        GetTextExtentPoint32W(dc, text.data(), static_cast<int>(text.size()), &size);
        SelectObject(dc, old_font);
        return size;
    }

    void draw_dot(HDC dc, const int cx, const int cy, const int diameter, const COLORREF color) {
        const RECT rc{cx - diameter / 2, cy - diameter / 2, cx - diameter / 2 + diameter, cy - diameter / 2 + diameter};
        HBRUSH brush = CreateSolidBrush(color);
        HPEN pen = CreatePen(PS_SOLID, 1, color);
        auto old_brush = SelectObject(dc, brush);
        auto old_pen = SelectObject(dc, pen);
        Ellipse(dc, rc.left, rc.top, rc.right, rc.bottom);
        SelectObject(dc, old_brush);
        SelectObject(dc, old_pen);
        DeleteObject(brush);
        DeleteObject(pen);
    }

    void draw_button(const DRAWITEMSTRUCT& di, const ButtonStyle style, const wchar_t icon, const UINT dpi) {
        const auto& p = palette();
        const auto& f = fonts(dpi);
        HDC dc = di.hDC;
        RECT rc = di.rcItem;
        const bool disabled = (di.itemState & ODS_DISABLED) != 0;
        const bool pressed = (di.itemState & ODS_SELECTED) != 0;
        const bool focused = (di.itemState & ODS_FOCUS) != 0 && (di.itemState & ODS_NOFOCUSRECT) == 0;
        HWND parent = GetParent(di.hwndItem);
        // the area around the rounded corners takes the parent's background
        SendMessageW(parent, WM_CTLCOLORBTN, reinterpret_cast<WPARAM>(dc), reinterpret_cast<LPARAM>(di.hwndItem));
        const COLORREF behind = GetBkColor(dc);
        fill_rect(dc, rc, behind);
        const std::wstring text = window_text(di.hwndItem);
        COLORREF fg = p.text;
        if (style == ButtonStyle::Link) {
            fg = disabled ? p.disabled_fg : p.link;
            HFONT font = f.ui;
            LOGFONTW lf{};
            GetObjectW(font, sizeof(lf), &lf);
            lf.lfUnderline = TRUE;
            HFONT underlined = CreateFontIndirectW(&lf);
            draw_text(dc, text, rc, fg, underlined, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            DeleteObject(underlined);
        } else {
            COLORREF bg = style == ButtonStyle::Accent ? p.accent : p.ctrl_bg;
            COLORREF border = style == ButtonStyle::Accent ? p.accent : p.ctrl_border;
            fg = style == ButtonStyle::Accent ? p.on_accent : p.text;
            if (disabled) {
                bg = style == ButtonStyle::Accent ? p.disabled_bg : p.ctrl_bg;
                border = style == ButtonStyle::Accent ? p.disabled_bg : p.ctrl_border;
                fg = p.disabled_fg;
            } else if (pressed) {
                bg = style == ButtonStyle::Accent ? RGB(GetRValue(p.accent) * 85 / 100, GetGValue(p.accent) * 85 / 100,
                                                        GetBValue(p.accent) * 85 / 100)
                                                  : p.hover;
            }
            fill_round_rect(dc, rc, scale(4, dpi), bg, border);
            HFONT font = style == ButtonStyle::Accent ? f.ui_semibold : f.ui;
            const SIZE ts = measure_text(dc, text, font);
            const int icon_w = icon ? scale(14, dpi) + scale(6, dpi) : 0;
            const int total = ts.cx + icon_w;
            int x = rc.left + (rc.right - rc.left - total) / 2;
            if (icon) {
                const RECT ir{x, rc.top, x + scale(14, dpi), rc.bottom};
                draw_glyph(dc, icon, ir, fg, f.icons_small);
                x += icon_w;
            }
            const RECT tr{x, rc.top, rc.right, rc.bottom};
            draw_text(dc, text, tr, fg, font, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        if (focused) {
            RECT fr = rc;
            InflateRect(&fr, -scale(3, dpi), -scale(3, dpi));
            SetTextColor(dc, fg);
            DrawFocusRect(dc, &fr);
        }
    }

    std::wstring widen(const std::string_view utf8) {
        if (utf8.empty()) return {};
        const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        std::wstring out(static_cast<std::size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
        return out;
    }

    std::string narrow(const std::wstring_view text) {
        if (text.empty()) return {};
        const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        std::string out(static_cast<std::size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
        return out;
    }

    std::wstring format_bytes(const std::uint64_t bytes) {
        wchar_t text[64] = {};
        if (FAILED(StrFormatByteSizeEx(bytes, SFBS_FLAGS_ROUND_TO_NEAREST_DISPLAYED_DIGIT, text, 64))) {
            swprintf(text, 64, L"%llu bytes", static_cast<unsigned long long>(bytes));
        }
        return text;
    }

    std::wstring format_duration(const std::chrono::milliseconds ms) {
        const auto total = ms.count() / 1000;
        wchar_t text[64];
        if (total < 60) swprintf(text, 64, L"%lld s", static_cast<long long>(total));
        else if (total < 3600) swprintf(text, 64, L"%lld min %lld s", static_cast<long long>(total / 60), static_cast<long long>(total % 60));
        else swprintf(text, 64, L"%lld h %lld min", static_cast<long long>(total / 3600), static_cast<long long>(total % 3600 / 60));
        return text;
    }

    std::wstring format_percent(const double percent) {
        wchar_t text[32];
        swprintf(text, 32, L"%.1f%%", percent);
        return text;
    }

    Category category_for(const std::string& extension) {
        // the badges have the design's six colors; font, scientific, database and unknown share the neutral one
        using chisel_app::FileKind;
        switch (chisel_app::file_kind(extension)) {
            case FileKind::Image: return CatImage;
            case FileKind::Audio: return CatAudio;
            case FileKind::Video: return CatVideo;
            case FileKind::Archive: return CatArchive;
            case FileKind::Document: return CatDoc;
            default: return CatApp;
        }
    }

    std::wstring badge_text(const std::string& extension) {
        std::wstring text = widen(extension.size() > 4 ? extension.substr(0, 4) : extension);
        std::ranges::transform(text, text.begin(), [](const wchar_t c) { return static_cast<wchar_t>(towupper(c)); });
        return text.empty() ? L"FILE" : text;
    }

    std::wstring window_text(HWND hwnd) {
        const int n = GetWindowTextLengthW(hwnd);
        std::wstring text(static_cast<std::size_t>(n) + 1, L'\0');
        GetWindowTextW(hwnd, text.data(), n + 1);
        text.resize(static_cast<std::size_t>(n));
        return text;
    }

    bool copy_to_clipboard(HWND owner, const std::wstring& text) {
        if (!OpenClipboard(owner)) return false;
        EmptyClipboard();
        const std::size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
        bool ok = false;
        if (mem) {
            void* dst = GlobalLock(mem);
            memcpy(dst, text.c_str(), bytes);
            GlobalUnlock(mem);
            ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
            if (!ok) GlobalFree(mem);
        }
        CloseClipboard();
        return ok;
    }

} // namespace chisel_win
