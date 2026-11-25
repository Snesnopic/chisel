#include "settings_window.hpp"
#include "i18n.hpp"
#include "platform.hpp"
#include "ui.hpp"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <string>
#include <thread>
#include <vector>

namespace chisel_win {

    namespace {
        enum : int {
            ID_APPLY = 3, ID_BROWSE = 300, ID_RESET, ID_FOLDER, ID_ITER, ID_ITER_LARGE, ID_ITER_SPIN, ID_ITER_LARGE_SPIN,
            ID_THREADS, ID_THREADS_VALUE, ID_THREADS_NOTE, ID_ITER_WARN,
            ID_MODE_OVERWRITE = 320, ID_MODE_KEEP, ID_MODE_SIDE, ID_MODE_FOLDER,
            ID_SUBFOLDERS = 330, ID_HIDE, ID_PRESERVE, ID_VERIFY, ID_SIGNED, ID_COPY_UNCHANGED, ID_KEEP_JPEG, ID_FLAC_EXHAUSTIVE,
            ID_LABEL_BASE = 400 // a label's id is its control's id plus this, so clicking it toggles the control
        };

        enum class Role { Heading, Text, Desc, Warn, Input };

        constexpr int kWidth = 640, kHeight = 600, kTabH = 44, kFooterH = 60, kLeft = 24, kRight = 616;
        constexpr DWORD kStyle = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;

        struct Control {
            HWND hwnd = nullptr;
            Role role = Role::Text;
            int tab = 0; // -1: every tab
            RECT at{};   // at 96 dpi, in client coordinates
        };

        struct SettingsWindow {
            HINSTANCE instance = nullptr;
            HWND hwnd = nullptr;
            HWND owner = nullptr;
            UINT dpi = 96;
            chisel_app::Settings working;
            chisel_app::Settings applied;
            bool locked = false;
            bool done = false;
            int tab = 0;
            int height = kHeight; // at 96 dpi, taller when translated text needs it
            unsigned max_threads = 1;
            const std::function<void(const chisel_app::Settings&)>* apply = nullptr;
            std::vector<Control> controls;
            RECT tab_rc[3]{};
            HBRUSH surface_brush = nullptr;
            HBRUSH ctrl_brush = nullptr;
            bool syncing = false;

            [[nodiscard]] int s(const int px) const { return scale(px, dpi); }

            HWND add(const wchar_t* cls, const wchar_t* text, const DWORD style, const int id, const Role role, const int on_tab,
                     const RECT at, const DWORD ex = 0) {
                HWND w = CreateWindowExW(ex, cls, text, WS_CHILD | style, 0, 0, 0, 0, hwnd,
                                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
                controls.push_back({w, role, on_tab, at});
                return w;
            }

            HWND label(const wchar_t* text, const int id, const Role role, const int on_tab, const RECT at) {
                return add(WC_STATICW, text, SS_LEFT | SS_NOTIFY | (role == Role::Desc || role == Role::Warn ? 0 : SS_ENDELLIPSIS), id,
                           role, on_tab, at);
            }

            void option(const wchar_t* title, const wchar_t* desc, const int id, const bool radio, const bool first,
                        const int on_tab, const int y, const int desc_lines) {
                const DWORD style = WS_TABSTOP | (radio ? BS_AUTORADIOBUTTON : BS_AUTOCHECKBOX) | (radio && first ? WS_GROUP : 0);
                add(WC_BUTTONW, L"", style, id, Role::Input, on_tab, {kLeft, y, kLeft + 18, y + 20});
                label(title, id + ID_LABEL_BASE, Role::Text, on_tab, {kLeft + 26, y + 1, kRight, y + 19});
                if (desc) label(desc, id + ID_LABEL_BASE + 50, Role::Desc, on_tab, {kLeft + 26, y + 20, kRight, y + 20 + 16 * desc_lines});
            }

            void create_controls() {
                // general
                label(tr(L"Output"), 0, Role::Heading, 0, {kLeft, 62, kRight, 84});
                option(tr(L"Replace the original"), tr(L"The smaller file takes the original's place."), ID_MODE_OVERWRITE, true, true, 0, 94, 1);
                option(tr(L"Keep the original"), tr(L"The original is renamed to name.original.ext and the smaller file takes its name."),
                       ID_MODE_KEEP, true, false, 0, 140, 1);
                option(tr(L"Save next to the original"), tr(L"The smaller file is saved as name.compressed.ext."), ID_MODE_SIDE, true, false, 0, 186, 1);
                option(tr(L"Save to a folder"), tr(L"Smaller files go to this folder, in the folder layout they came from."), ID_MODE_FOLDER, true,
                       false, 0, 232, 1);
                add(WC_EDITW, L"", WS_TABSTOP | ES_AUTOHSCROLL | ES_READONLY | WS_BORDER, ID_FOLDER, Role::Input, 0,
                    {kLeft + 26, 278, kRight - 96, 306});
                add(WC_BUTTONW, tr(L"Browse…"), WS_TABSTOP | BS_OWNERDRAW, ID_BROWSE, Role::Input, 0, {kRight - 88, 278, kRight, 306});
                // belongs to "Save to a folder", so it sits under the folder and works only in that mode
                add(WC_BUTTONW, L"", WS_TABSTOP | BS_AUTOCHECKBOX, ID_COPY_UNCHANGED, Role::Input, 0, {kLeft + 26, 316, kLeft + 44, 336});
                label(tr(L"Copy files that didn't shrink to the output folder"), ID_COPY_UNCHANGED + ID_LABEL_BASE, Role::Text, 0,
                      {kLeft + 52, 317, kRight, 335});
                label(tr(L"Adding files"), 0, Role::Heading, 0, {kLeft, 352, kRight, 374});
                option(tr(L"Include subfolders when adding a folder"), nullptr, ID_SUBFOLDERS, false, false, 0, 384, 0);
                option(tr(L"Hide unsupported files inside containers"),
                       tr(L"PDFs, archives and Office documents often hold many files that can't be recompressed."), ID_HIDE, false, false,
                       0, 414, 1);

                // compression
                label(tr(L"Compression"), 0, Role::Heading, 1, {kLeft, 62, kRight, 84});
                option(tr(L"Preserve file metadata"),
                       tr(L"Keeps EXIF, XMP and color profiles. Turning it off gives slightly smaller files but strips most non-essential data."),
                       ID_PRESERVE, false, false, 1, 94, 2);
                option(tr(L"Verify data integrity"),
                       tr(L"After each compression, checks that the content matches the original exactly. Slower."), ID_VERIFY, false,
                       false, 1, 156, 1);
                option(tr(L"Optimize signed files"),
                       tr(L"Signed PDFs and Office documents, executables and photos with content credentials are left untouched so their "
                       L"signature stays valid. Turning this on breaks the signature."),
                       ID_SIGNED, false, false, 1, 202, 2);
                label(tr(L"Performance"), 0, Role::Heading, 1, {kLeft, 280, kRight, 302});
                label(tr(L"Files processed at the same time"), 0, Role::Text, 1, {kLeft, 312, kRight - 100, 330});
                add(WC_STATICW, L"", SS_RIGHT, ID_THREADS_VALUE, Role::Desc, 1, {kRight - 100, 312, kRight, 330});
                add(TRACKBAR_CLASSW, L"", WS_TABSTOP | TBS_HORZ | TBS_NOTICKS, ID_THREADS, Role::Input, 1, {kLeft - 4, 334, kRight + 4, 364});
                add(WC_STATICW, L"", SS_LEFT, ID_THREADS_NOTE, Role::Desc, 1, {kLeft, 368, kRight, 400});

                // advanced
                label(tr(L"Zopfli / Deflate"), 0, Role::Heading, 2, {kLeft, 62, kRight, 84});
                label(tr(L"Iterations of the zopfli algorithm on deflate data: PNG, PDF, ZIP and other archives. More iterations find smaller "
                      L"files, slowly."), 0, Role::Desc, 2, {kLeft, 88, kRight, 120});
                label(tr(L"Iterations"), 0, Role::Text, 2, {kLeft, 136, kRight - 136, 156});
                add(WC_EDITW, L"", WS_TABSTOP | ES_NUMBER | WS_BORDER, ID_ITER, Role::Input, 2, {kRight - 120, 132, kRight, 160});
                add(UPDOWN_CLASSW, L"", UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_NOTHOUSANDS, ID_ITER_SPIN, Role::Input, 2,
                    {kRight - 20, 132, kRight, 160});
                label(tr(L"Iterations on large files"), 0, Role::Text, 2, {kLeft, 176, kRight - 136, 196});
                add(WC_EDITW, L"", WS_TABSTOP | ES_NUMBER | WS_BORDER, ID_ITER_LARGE, Role::Input, 2, {kRight - 120, 172, kRight, 200});
                add(UPDOWN_CLASSW, L"", UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_NOTHOUSANDS, ID_ITER_LARGE_SPIN,
                    Role::Input, 2, {kRight - 20, 172, kRight, 200});
                add(WC_STATICW, tr(L"High values make processing much slower."), SS_LEFT, ID_ITER_WARN, Role::Warn, 2, {kLeft, 210, kRight, 228});
                add(WC_BUTTONW, tr(L"Reset to defaults"), WS_TABSTOP | BS_OWNERDRAW, ID_RESET, Role::Input, 2, {kLeft, 238, kLeft + 150, 270});
                label(tr(L"JPEG and FLAC"), 0, Role::Heading, 2, {kLeft, 292, kRight, 314});
                option(tr(L"Keep each JPEG baseline or progressive as it is"),
                       tr(L"Otherwise each JPEG becomes baseline or progressive, whichever is smaller. The image itself never changes."),
                       ID_KEEP_JPEG, false, false, 2, 324, 1);
                option(tr(L"Exhaustive FLAC search"),
                       tr(L"Slightly smaller FLAC files, at hundreds to thousands of times the encoding time."), ID_FLAC_EXHAUSTIVE, false,
                       false, 2, 370, 1);
                label(tr(L"Changes apply to the next run."), 0, Role::Desc, 2, {kLeft, 424, kRight, 442});

                // every tab
                if (locked) {
                    label(tr(L"Files are being processed: settings can't be changed until the run ends."), 0, Role::Warn, -1,
                          {kLeft, height - kFooterH - 30, kRight, height - kFooterH - 12});
                }
                const int by = height - kFooterH + 14;
                add(WC_BUTTONW, tr(L"OK"), WS_TABSTOP | BS_OWNERDRAW, IDOK, Role::Input, -1, {kRight + 4 - 3 * 88 - 16, by, kRight + 4 - 2 * 88 - 16, by + 32});
                add(WC_BUTTONW, tr(L"Cancel"), WS_TABSTOP | BS_OWNERDRAW, IDCANCEL, Role::Input, -1, {kRight + 4 - 2 * 88 - 8, by, kRight + 4 - 88 - 8, by + 32});
                add(WC_BUTTONW, tr(L"Apply"), WS_TABSTOP | BS_OWNERDRAW, ID_APPLY, Role::Input, -1, {kRight + 4 - 88, by, kRight + 4, by + 32});
            }

            HWND item(const int id) const { return GetDlgItem(hwnd, id); }

            // wrapped text gets the lines it needs and pushes down what's below it; the window grows if a tab runs out of room
            void fit_texts() {
                HDC dc = GetDC(hwnd);
                HGDIOBJ old = SelectObject(dc, fonts(dpi).ui_small);
                const auto lines_height = [&](const std::wstring& text, const RECT& at) {
                    RECT r{0, 0, s(at.right - at.left), 0};
                    DrawTextW(dc, text.c_str(), -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_EXPANDTABS);
                    return MulDiv(r.bottom, 96, static_cast<int>(dpi)) + 1;
                };
                for (Control& c : controls) {
                    if (c.role != Role::Desc && c.role != Role::Warn) continue;
                    int needed = 0;
                    if (c.hwnd == item(ID_THREADS_NOTE)) {
                        // refresh_derived switches between two texts
                        needed = std::max(lines_height(tr(L"Using every thread will make the system unresponsive while Chisel runs."), c.at),
                                          lines_height(tr(L"More files at once is faster but uses more CPU and memory. The default is half of your cores."), c.at));
                    } else if (const std::wstring text = window_text(c.hwnd); !text.empty()) {
                        needed = lines_height(text, c.at);
                    }
                    const int grow = needed - static_cast<int>(c.at.bottom - c.at.top);
                    if (grow <= 0) continue;
                    if (c.tab == -1) {
                        c.at.top -= grow;
                        continue;
                    }
                    const LONG old_bottom = c.at.bottom;
                    c.at.bottom += grow;
                    for (Control& other : controls) {
                        if (&other != &c && other.tab == c.tab && other.at.top >= old_bottom) OffsetRect(&other.at, 0, grow);
                    }
                }
                SelectObject(dc, old);
                ReleaseDC(hwnd, dc);
                if (HWND reset = item(ID_RESET)) {
                    for (Control& c : controls) {
                        if (c.hwnd != reset) continue;
                        const int needed = MulDiv(button_width(reset, ButtonStyle::Normal, false, dpi), 96, static_cast<int>(dpi)) + 1;
                        c.at.right = std::max<LONG>(c.at.right, c.at.left + needed);
                    }
                }

                LONG bottom = 0;
                LONG warning_top = height - kFooterH;
                for (const Control& c : controls) {
                    if (c.tab >= 0) bottom = std::max(bottom, c.at.bottom);
                    else if (c.role == Role::Warn) warning_top = std::min(warning_top, c.at.top);
                }
                const int extra = static_cast<int>(bottom - (warning_top - 12));
                if (extra <= 0) return;
                height += extra;
                for (Control& c : controls) {
                    if (c.tab == -1) OffsetRect(&c.at, 0, extra);
                }
                RECT rc{0, 0, s(kWidth), s(height)};
                AdjustWindowRectExForDpi(&rc, kStyle, FALSE, WS_EX_DLGMODALFRAME, dpi);
                RECT now;
                GetWindowRect(hwnd, &now);
                SetWindowPos(hwnd, nullptr, now.left, now.top - s(extra) / 2, rc.right - rc.left, rc.bottom - rc.top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }

            void load_values() {
                syncing = true;
                const int mode_id = ID_MODE_OVERWRITE + static_cast<int>(working.output_mode);
                for (int id = ID_MODE_OVERWRITE; id <= ID_MODE_FOLDER; ++id) Button_SetCheck(item(id), id == mode_id ? BST_CHECKED : BST_UNCHECKED);
                SetWindowTextW(item(ID_FOLDER), working.output_folder.wstring().c_str());
                Button_SetCheck(item(ID_SUBFOLDERS), working.recursive_folder_import ? BST_CHECKED : BST_UNCHECKED);
                Button_SetCheck(item(ID_HIDE), working.hide_unsupported ? BST_CHECKED : BST_UNCHECKED);
                Button_SetCheck(item(ID_PRESERVE), working.preserve_metadata ? BST_CHECKED : BST_UNCHECKED);
                Button_SetCheck(item(ID_VERIFY), working.verify_checksums ? BST_CHECKED : BST_UNCHECKED);
                Button_SetCheck(item(ID_SIGNED), working.break_signatures ? BST_CHECKED : BST_UNCHECKED);
                Button_SetCheck(item(ID_COPY_UNCHANGED), working.copy_unchanged ? BST_CHECKED : BST_UNCHECKED);
                Button_SetCheck(item(ID_KEEP_JPEG), working.progressive_jpeg ? BST_UNCHECKED : BST_CHECKED);
                Button_SetCheck(item(ID_FLAC_EXHAUSTIVE), working.exhaustive_flac ? BST_CHECKED : BST_UNCHECKED);
                SendMessageW(item(ID_THREADS), TBM_SETRANGE, TRUE, MAKELPARAM(1, max_threads));
                SendMessageW(item(ID_THREADS), TBM_SETPOS, TRUE, static_cast<LPARAM>(std::min(working.threads, max_threads)));
                SendMessageW(item(ID_ITER_SPIN), UDM_SETRANGE32, 1, 1000);
                SendMessageW(item(ID_ITER_LARGE_SPIN), UDM_SETRANGE32, 1, 1000);
                SetWindowTextW(item(ID_ITER), std::to_wstring(working.iterations).c_str());
                SetWindowTextW(item(ID_ITER_LARGE), std::to_wstring(working.iterations_large).c_str());
                syncing = false;
                refresh_derived();
            }

            void refresh_derived() {
                SetWindowTextW(item(ID_THREADS_VALUE), trf(L"{0} of {1}", {std::to_wstring(working.threads), std::to_wstring(max_threads)}).c_str());
                const bool all = working.threads >= max_threads;
                SetWindowTextW(item(ID_THREADS_NOTE), all ? tr(L"Using every thread will make the system unresponsive while Chisel runs.")
                                                          : tr(L"More files at once is faster but uses more CPU and memory. The default is half of your cores."));
                for (auto& c : controls) {
                    if (c.hwnd == item(ID_THREADS_NOTE)) c.role = all ? Role::Warn : Role::Desc;
                }
                InvalidateRect(item(ID_THREADS_NOTE), nullptr, TRUE);
                const bool warn = working.iterations > 50 || working.iterations_large > 50;
                ShowWindow(item(ID_ITER_WARN), warn && tab == 2 ? SW_SHOW : SW_HIDE);
                const bool folder = working.output_mode == chisel_app::OutputMode::OutputFolder;
                EnableWindow(item(ID_FOLDER), folder && !locked);
                EnableWindow(item(ID_BROWSE), folder && !locked);
                EnableWindow(item(ID_COPY_UNCHANGED), folder && !locked);
                EnableWindow(item(ID_COPY_UNCHANGED + ID_LABEL_BASE), folder && !locked);
                EnableWindow(item(ID_APPLY), !locked && !(working == applied));
            }

            void read_values() {
                if (syncing) return;
                for (int id = ID_MODE_OVERWRITE; id <= ID_MODE_FOLDER; ++id) {
                    if (Button_GetCheck(item(id)) == BST_CHECKED) working.output_mode = static_cast<chisel_app::OutputMode>(id - ID_MODE_OVERWRITE);
                }
                working.recursive_folder_import = Button_GetCheck(item(ID_SUBFOLDERS)) == BST_CHECKED;
                working.hide_unsupported = Button_GetCheck(item(ID_HIDE)) == BST_CHECKED;
                working.preserve_metadata = Button_GetCheck(item(ID_PRESERVE)) == BST_CHECKED;
                working.verify_checksums = Button_GetCheck(item(ID_VERIFY)) == BST_CHECKED;
                working.break_signatures = Button_GetCheck(item(ID_SIGNED)) == BST_CHECKED;
                working.copy_unchanged = Button_GetCheck(item(ID_COPY_UNCHANGED)) == BST_CHECKED;
                working.progressive_jpeg = Button_GetCheck(item(ID_KEEP_JPEG)) != BST_CHECKED;
                working.exhaustive_flac = Button_GetCheck(item(ID_FLAC_EXHAUSTIVE)) == BST_CHECKED;
                working.threads = static_cast<unsigned>(std::max<LRESULT>(1, SendMessageW(item(ID_THREADS), TBM_GETPOS, 0, 0)));
                const auto number = [&](const int id, const unsigned fallback) {
                    const std::wstring text = window_text(item(id));
                    const unsigned long value = text.empty() ? 0 : std::wcstoul(text.c_str(), nullptr, 10);
                    return value >= 1 && value <= 1000 ? static_cast<unsigned>(value) : fallback;
                };
                working.iterations = number(ID_ITER, working.iterations);
                working.iterations_large = number(ID_ITER_LARGE, working.iterations_large);
                refresh_derived();
            }

            void show_tab(const int which) {
                tab = which;
                for (const auto& c : controls) ShowWindow(c.hwnd, c.tab == -1 || c.tab == tab ? SW_SHOW : SW_HIDE);
                refresh_derived();
                InvalidateRect(hwnd, nullptr, FALSE);
            }

            void layout() {
                for (const auto& c : controls) {
                    SetWindowPos(c.hwnd, nullptr, s(c.at.left), s(c.at.top), s(c.at.right - c.at.left), s(c.at.bottom - c.at.top),
                                 SWP_NOZORDER | SWP_NOACTIVATE);
                    const auto& f = fonts(dpi);
                    HFONT font = c.role == Role::Heading ? f.heading : (c.role == Role::Desc || c.role == Role::Warn) ? f.ui_small : f.ui;
                    SendMessageW(c.hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
                }
                // an up-down control sits inside its buddy, which just moved
                SendMessageW(item(ID_ITER_SPIN), UDM_SETBUDDY, reinterpret_cast<WPARAM>(item(ID_ITER)), 0);
                SendMessageW(item(ID_ITER_LARGE_SPIN), UDM_SETBUDDY, reinterpret_cast<WPARAM>(item(ID_ITER_LARGE)), 0);
                HDC dc = GetDC(hwnd);
                int x = s(16);
                const wchar_t* names[] = {tr(L"General"), tr(L"Compression"), tr(L"Advanced")};
                for (int i = 0; i < 3; ++i) {
                    const int w = measure_text(dc, names[i], fonts(dpi).ui_semibold).cx + s(24);
                    tab_rc[i] = {x, 0, x + w, s(kTabH)};
                    x += w + s(4);
                }
                ReleaseDC(hwnd, dc);
            }

            void apply_theme() {
                apply_window_theme(hwnd);
                if (surface_brush) DeleteObject(surface_brush);
                if (ctrl_brush) DeleteObject(ctrl_brush);
                surface_brush = CreateSolidBrush(palette().surface);
                ctrl_brush = CreateSolidBrush(palette().ctrl_bg);
                for (const auto& c : controls) {
                    wchar_t cls[32] = {};
                    GetClassNameW(c.hwnd, cls, 32);
                    if (lstrcmpiW(cls, WC_STATICW) != 0) apply_control_theme(c.hwnd);
                }
                RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
            }

            void paint(HDC dc) {
                const auto& p = palette();
                const auto& f = fonts(dpi);
                RECT rc;
                GetClientRect(hwnd, &rc);
                fill_rect(dc, {0, 0, rc.right, s(kTabH)}, p.window);
                fill_rect(dc, {0, s(kTabH) - 1, rc.right, s(kTabH)}, p.border);
                fill_rect(dc, {0, s(kTabH), rc.right, rc.bottom - s(kFooterH)}, p.surface);
                fill_rect(dc, {0, rc.bottom - s(kFooterH), rc.right, rc.bottom}, p.window);
                fill_rect(dc, {0, rc.bottom - s(kFooterH), rc.right, rc.bottom - s(kFooterH) + 1}, p.border);
                const wchar_t* names[] = {tr(L"General"), tr(L"Compression"), tr(L"Advanced")};
                for (int i = 0; i < 3; ++i) {
                    const bool on = i == tab;
                    draw_text(dc, names[i], tab_rc[i], on ? p.text : p.text2, on ? f.ui_semibold : f.ui, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    if (on) fill_rect(dc, {tab_rc[i].left, tab_rc[i].bottom - s(2), tab_rc[i].right, tab_rc[i].bottom}, p.accent);
                }
            }

            Role role_of(HWND w) const {
                for (const auto& c : controls) {
                    if (c.hwnd == w) return c.role;
                }
                return Role::Text;
            }

            void commit() {
                if (locked || working == applied) return;
                applied = working;
                if (apply && *apply) (*apply)(applied);
                refresh_derived();
            }

            LRESULT handle(const UINT msg, const WPARAM wp, const LPARAM lp) {
                switch (msg) {
                    case WM_ERASEBKGND:
                        return 1;
                    case WM_PAINT: {
                        PAINTSTRUCT ps;
                        HDC dc = BeginPaint(hwnd, &ps);
                        paint(dc);
                        EndPaint(hwnd, &ps);
                        return 0;
                    }
                    case WM_LBUTTONDOWN: {
                        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                        for (int i = 0; i < 3; ++i) {
                            if (PtInRect(&tab_rc[i], pt)) show_tab(i);
                        }
                        return 0;
                    }
                    case WM_CTLCOLORSTATIC: {
                        HDC dc = reinterpret_cast<HDC>(wp);
                        const auto& p = palette();
                        const Role role = role_of(reinterpret_cast<HWND>(lp));
                        SetTextColor(dc, role == Role::Desc ? p.text2 : role == Role::Warn ? p.warn : p.text);
                        if (reinterpret_cast<HWND>(lp) == item(ID_FOLDER)) {
                            SetBkColor(dc, p.ctrl_bg);
                            return reinterpret_cast<LRESULT>(ctrl_brush);
                        }
                        SetBkColor(dc, p.surface);
                        return reinterpret_cast<LRESULT>(surface_brush);
                    }
                    case WM_CTLCOLOREDIT: {
                        HDC dc = reinterpret_cast<HDC>(wp);
                        SetTextColor(dc, palette().text);
                        SetBkColor(dc, palette().ctrl_bg);
                        return reinterpret_cast<LRESULT>(ctrl_brush);
                    }
                    case WM_CTLCOLORBTN: {
                        HDC dc = reinterpret_cast<HDC>(wp);
                        const int id = GetDlgCtrlID(reinterpret_cast<HWND>(lp));
                        const COLORREF bg = id == IDOK || id == IDCANCEL || id == ID_APPLY ? palette().window : palette().surface;
                        SetBkColor(dc, bg);
                        SetDCBrushColor(dc, bg);
                        return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
                    }
                    case WM_DRAWITEM: {
                        const auto* di = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
                        draw_button(*di, di->CtlID == IDOK ? ButtonStyle::Accent : ButtonStyle::Normal, 0, dpi);
                        return TRUE;
                    }
                    case WM_HSCROLL:
                        if (reinterpret_cast<HWND>(lp) == item(ID_THREADS)) read_values();
                        return 0;
                    case WM_COMMAND: {
                        const int id = LOWORD(wp);
                        const int code = HIWORD(wp);
                        if (id == IDOK) {
                            commit();
                            done = true;
                        } else if (id == IDCANCEL) {
                            done = true;
                        } else if (id == ID_APPLY) {
                            commit();
                        } else if (id == ID_BROWSE) {
                            const auto folders = pick_folders(hwnd, false);
                            if (!folders.empty()) {
                                working.output_folder = folders.front();
                                SetWindowTextW(item(ID_FOLDER), working.output_folder.wstring().c_str());
                                refresh_derived();
                            }
                        } else if (id == ID_RESET) {
                            const chisel_app::Settings defaults;
                            SetWindowTextW(item(ID_ITER), std::to_wstring(defaults.iterations).c_str());
                            SetWindowTextW(item(ID_ITER_LARGE), std::to_wstring(defaults.iterations_large).c_str());
                            read_values();
                        } else if (id >= ID_LABEL_BASE && code == STN_CLICKED) {
                            // a click on an option's title (control id + 400) or description (+ 450) works like a click on its box
                            const int control = id >= ID_LABEL_BASE + 50 + ID_MODE_OVERWRITE ? id - ID_LABEL_BASE - 50 : id - ID_LABEL_BASE;
                            HWND box = item(control);
                            if (box && IsWindowEnabled(box)) {
                                const LONG style = GetWindowLongW(box, GWL_STYLE) & BS_TYPEMASK;
                                if (style == BS_AUTORADIOBUTTON) {
                                    for (int r = ID_MODE_OVERWRITE; r <= ID_MODE_FOLDER; ++r) Button_SetCheck(item(r), item(r) == box ? BST_CHECKED : BST_UNCHECKED);
                                } else {
                                    Button_SetCheck(box, Button_GetCheck(box) == BST_CHECKED ? BST_UNCHECKED : BST_CHECKED);
                                }
                                SetFocus(box);
                                read_values();
                            }
                        } else if (code == BN_CLICKED || code == EN_CHANGE) {
                            read_values();
                        }
                        return 0;
                    }
                    case WM_DPICHANGED: {
                        dpi = HIWORD(wp);
                        const auto* r = reinterpret_cast<RECT*>(lp);
                        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
                        layout();
                        InvalidateRect(hwnd, nullptr, TRUE);
                        return 0;
                    }
                    case WM_SETTINGCHANGE:
                        if (lp && wcscmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0) {
                            refresh_theme();
                            apply_theme();
                        }
                        return 0;
                    case WM_CLOSE:
                        done = true;
                        return 0;
                    default:
                        break;
                }
                return DefWindowProcW(hwnd, msg, wp, lp);
            }

            static LRESULT CALLBACK proc(HWND hwnd, const UINT msg, const WPARAM wp, const LPARAM lp) {
                SettingsWindow* self = nullptr;
                if (msg == WM_NCCREATE) {
                    self = static_cast<SettingsWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
                    self->hwnd = hwnd;
                    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
                } else {
                    self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
                }
                return self ? self->handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
            }
        };
    } // namespace

    void run_settings_window(HWND owner, HINSTANCE instance, const chisel_app::Settings& current, const bool locked,
                             const std::function<void(const chisel_app::Settings&)>& apply) {
        static bool registered = false;
        if (!registered) {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = SettingsWindow::proc;
            wc.hInstance = instance;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.lpszClassName = L"ChiselSettings";
            RegisterClassExW(&wc);
            registered = true;
        }
        SettingsWindow w;
        w.instance = instance;
        w.owner = owner;
        w.working = current;
        w.applied = current;
        w.locked = locked;
        w.apply = &apply;
        w.max_threads = std::max({1u, std::thread::hardware_concurrency(), current.threads});
        w.dpi = GetDpiForWindow(owner);

        RECT rc{0, 0, scale(kWidth, w.dpi), scale(kHeight, w.dpi)};
        AdjustWindowRectExForDpi(&rc, kStyle, FALSE, WS_EX_DLGMODALFRAME, w.dpi);
        RECT orc;
        GetWindowRect(owner, &orc);
        const int width = rc.right - rc.left;
        const int height = rc.bottom - rc.top;
        const int x = orc.left + (orc.right - orc.left - width) / 2;
        const int y = orc.top + (orc.bottom - orc.top - height) / 2;
        CreateWindowExW(WS_EX_DLGMODALFRAME, L"ChiselSettings", tr(L"Settings"), kStyle, x, y, width, height, owner, nullptr, instance, &w);
        if (!w.hwnd) return;
        w.create_controls();
        w.fit_texts();
        w.layout();
        w.apply_theme();
        w.load_values();
        if (locked) {
            for (const auto& c : w.controls) {
                const int id = GetDlgCtrlID(c.hwnd);
                if (c.role == Role::Input && id != IDOK && id != IDCANCEL) EnableWindow(c.hwnd, FALSE);
            }
        }
        w.show_tab(0);
        EnableWindow(owner, FALSE);
        ShowWindow(w.hwnd, SW_SHOW);
        SetFocus(GetDlgItem(w.hwnd, IDOK));

        MSG msg;
        while (!w.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (msg.message == WM_KEYDOWN && msg.wParam == VK_TAB && (GetKeyState(VK_CONTROL) & 0x8000)) {
                const bool back = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                w.show_tab((w.tab + (back ? 2 : 1)) % 3);
                continue;
            }
            if (IsDialogMessageW(w.hwnd, &msg)) continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (msg.message == WM_QUIT) PostQuitMessage(static_cast<int>(msg.wParam));
        EnableWindow(owner, TRUE);
        SetActiveWindow(owner);
        DestroyWindow(w.hwnd);
        if (w.surface_brush) DeleteObject(w.surface_brush);
        if (w.ctrl_brush) DeleteObject(w.ctrl_brush);
    }

} // namespace chisel_win
