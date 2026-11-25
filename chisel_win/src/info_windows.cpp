#include "info_windows.hpp"
#include "i18n.hpp"
#include "ui.hpp"

#include "chisel_app/formats.hpp"

#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>

#ifndef CHISEL_VERSION
#define CHISEL_VERSION "dev"
#endif

namespace chisel_win {

    namespace {
        constexpr const wchar_t* kRepository = L"https://github.com/Snesnopic/chisel";
        constexpr const wchar_t* kIconAuthor = L"https://www.behance.net/mychan1";

        std::wstring kind_text(const chisel_app::FileKind kind) {
            using chisel_app::FileKind;
            switch (kind) {
                case FileKind::Archive: return tr(L"Archive");
                case FileKind::Audio: return tr(L"Audio");
                case FileKind::Database: return tr(L"Database");
                case FileKind::Document: return tr(L"Document");
                case FileKind::Font: return tr(L"Font");
                case FileKind::Image: return tr(L"Image");
                case FileKind::Scientific: return tr(L"Scientific");
                case FileKind::Video: return tr(L"Video");
                case FileKind::Unknown: break;
            }
            return tr(L"Unknown");
        }
        enum : int { ID_LINK = 600, ID_CREDIT, ID_FORMATS };
        constexpr int kAboutW = 440, kAboutH = 300;

        // disables the owner and pumps messages until done; Esc closes
        void run_modal(HWND owner, HWND window, const bool& done) {
            EnableWindow(owner, FALSE);
            ShowWindow(window, SW_SHOW);
            MSG msg{};
            while (!done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
                if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE && (msg.hwnd == window || IsChild(window, msg.hwnd))) {
                    PostMessageW(window, WM_CLOSE, 0, 0);
                    continue;
                }
                if (IsDialogMessageW(window, &msg)) continue;
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (msg.message == WM_QUIT) PostQuitMessage(static_cast<int>(msg.wParam));
            EnableWindow(owner, TRUE);
            SetActiveWindow(owner);
            DestroyWindow(window);
        }

        HWND create_centered(HWND owner, HINSTANCE instance, const wchar_t* cls, const wchar_t* title, const DWORD style,
                             const int width, const int height, const UINT dpi, void* self) {
            RECT rc{0, 0, scale(width, dpi), scale(height, dpi)};
            AdjustWindowRectExForDpi(&rc, style, FALSE, WS_EX_DLGMODALFRAME, dpi);
            RECT orc;
            GetWindowRect(owner, &orc);
            const int w = rc.right - rc.left;
            const int h = rc.bottom - rc.top;
            return CreateWindowExW(WS_EX_DLGMODALFRAME, cls, title, style, orc.left + (orc.right - orc.left - w) / 2,
                                   orc.top + (orc.bottom - orc.top - h) / 2, w, h, owner, nullptr, instance, self);
        }

        void register_class(HINSTANCE instance, const wchar_t* name, WNDPROC proc) {
            WNDCLASSEXW wc{};
            if (GetClassInfoExW(instance, name, &wc)) return;
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = proc;
            wc.hInstance = instance;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.lpszClassName = name;
            RegisterClassExW(&wc);
        }

        template <typename T>
        T* self_of(HWND hwnd, const UINT msg, const LPARAM lp) {
            if (msg == WM_NCCREATE) {
                auto* self = static_cast<T*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
                self->hwnd = hwnd;
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
                return self;
            }
            return reinterpret_cast<T*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }

        struct AboutWindow {
            HINSTANCE instance = nullptr;
            HWND hwnd = nullptr;
            HWND link = nullptr;
            HWND credit = nullptr;
            HWND formats = nullptr;
            HWND ok = nullptr;
            HICON icon = nullptr;
            UINT dpi = 96;
            bool done = false;
            int credit_x = 0;
            const std::vector<std::string>* extensions = nullptr;

            [[nodiscard]] int s(const int px) const { return scale(px, dpi); }

            void layout() {
                HDC dc = GetDC(hwnd);
                const auto width = [&](HWND w) { return static_cast<int>(measure_text(dc, window_text(w), fonts(dpi).ui).cx) + s(4); };
                credit_x = s(24) + measure_text(dc, std::wstring(tr(L"App icon by")) + L" ", fonts(dpi).ui).cx;
                SetWindowPos(link, nullptr, s(22), s(158), width(link), s(24), SWP_NOZORDER);
                SetWindowPos(credit, nullptr, credit_x - s(2), s(186), width(credit), s(24), SWP_NOZORDER);
                SetWindowPos(formats, nullptr, s(22), s(214), width(formats), s(24), SWP_NOZORDER);
                ReleaseDC(hwnd, dc);
                SetWindowPos(ok, nullptr, s(kAboutW - 24 - 88), s(kAboutH - 24 - 32), s(88), s(32), SWP_NOZORDER);
                if (icon) DestroyIcon(icon);
                icon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, s(64), s(64), 0));
            }

            void paint(HDC dc) {
                const auto& p = palette();
                const auto& f = fonts(dpi);
                RECT rc;
                GetClientRect(hwnd, &rc);
                fill_rect(dc, rc, p.window);
                if (icon) DrawIconEx(dc, s(24), s(24), icon, s(64), s(64), 0, nullptr, DI_NORMAL);
                draw_text(dc, L"Chisel", {s(104), s(26), rc.right - s(24), s(56)}, p.text, f.title, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                draw_text(dc, trf(L"Version {0}", {widen(CHISEL_VERSION)}), {s(104), s(58), rc.right - s(24), s(78)}, p.text2, f.ui,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                draw_text(dc, tr(L"Lossless recompression of images, audio, documents and archives, and of the files inside them."),
                          {s(24), s(106), rc.right - s(24), s(150)}, p.text, f.ui, DT_LEFT | DT_WORDBREAK);
                draw_text(dc, std::wstring(tr(L"App icon by")) + L" ", {s(24), s(186), credit_x, s(210)}, p.text, f.ui, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
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
                    case WM_SETCURSOR:
                        if (reinterpret_cast<HWND>(wp) == link || reinterpret_cast<HWND>(wp) == credit ||
                            reinterpret_cast<HWND>(wp) == formats) {
                            SetCursor(LoadCursorW(nullptr, IDC_HAND));
                            return TRUE;
                        }
                        break;
                    case WM_CTLCOLORBTN: {
                        HDC dc = reinterpret_cast<HDC>(wp);
                        SetBkColor(dc, palette().window);
                        SetDCBrushColor(dc, palette().window);
                        return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
                    }
                    case WM_DRAWITEM: {
                        const auto* di = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
                        draw_button(*di, di->CtlID == IDOK ? ButtonStyle::Accent : ButtonStyle::Link, 0, dpi);
                        return TRUE;
                    }
                    case WM_COMMAND:
                        if (LOWORD(wp) == ID_LINK) ShellExecuteW(hwnd, L"open", kRepository, nullptr, nullptr, SW_SHOWNORMAL);
                        else if (LOWORD(wp) == ID_CREDIT) ShellExecuteW(hwnd, L"open", kIconAuthor, nullptr, nullptr, SW_SHOWNORMAL);
                        else if (LOWORD(wp) == ID_FORMATS && extensions) show_formats_window(hwnd, instance, *extensions);
                        else if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) done = true;
                        return 0;
                    case WM_DPICHANGED: {
                        dpi = HIWORD(wp);
                        const auto* r = reinterpret_cast<RECT*>(lp);
                        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
                        layout();
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }
                    case WM_CLOSE:
                        done = true;
                        return 0;
                    default:
                        break;
                }
                return DefWindowProcW(hwnd, msg, wp, lp);
            }

            static LRESULT CALLBACK proc(HWND hwnd, const UINT msg, const WPARAM wp, const LPARAM lp) {
                auto* self = self_of<AboutWindow>(hwnd, msg, lp);
                return self ? self->handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
            }
        };

        struct FormatsWindow {
            struct Section {
                std::wstring name;
                std::wstring count;
                std::wstring list;
                Category badge = CatApp;
                int top = 0;
                int list_height = 0;
            };

            HWND hwnd = nullptr;
            UINT dpi = 96;
            bool done = false;
            std::vector<Section> sections;
            std::wstring summary;
            int content_height = 0;
            int pos = 0;

            [[nodiscard]] int s(const int px) const { return scale(px, dpi); }

            void measure() {
                RECT rc;
                GetClientRect(hwnd, &rc);
                const auto& f = fonts(dpi);
                HDC dc = GetDC(hwnd);
                int y = s(84);
                for (auto& section : sections) {
                    section.top = y;
                    RECT text{s(24), 0, std::max(s(100), static_cast<int>(rc.right) - s(24)), 0};
                    auto old = SelectObject(dc, f.ui);
                    DrawTextW(dc, section.list.c_str(), static_cast<int>(section.list.size()), &text, DT_LEFT | DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
                    SelectObject(dc, old);
                    section.list_height = text.bottom - text.top;
                    y += s(32) + section.list_height + s(20);
                }
                ReleaseDC(hwnd, dc);
                content_height = y;
                SCROLLINFO si{sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS, 0, std::max(0, content_height - 1),
                              static_cast<UINT>(rc.bottom), pos, 0};
                SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
                scroll_to(pos);
            }

            void scroll_to(const int target) {
                RECT rc;
                GetClientRect(hwnd, &rc);
                const int top = std::clamp(target, 0, std::max(0, content_height - static_cast<int>(rc.bottom)));
                if (top == pos) return;
                pos = top;
                SetScrollPos(hwnd, SB_VERT, pos, TRUE);
                InvalidateRect(hwnd, nullptr, FALSE);
            }

            void paint(HDC dc) {
                const auto& p = palette();
                const auto& f = fonts(dpi);
                RECT rc;
                GetClientRect(hwnd, &rc);
                fill_rect(dc, rc, p.surface);
                const int dy = -pos;
                draw_text(dc, tr(L"Supported formats"), {s(24), s(18) + dy, rc.right - s(24), s(48) + dy}, p.text, f.title,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                draw_text(dc, summary, {s(24), s(50) + dy, rc.right - s(24), s(70) + dy}, p.text2, f.ui, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                for (const auto& section : sections) {
                    const int y = section.top + dy;
                    if (y > rc.bottom || y + s(32) + section.list_height < 0) continue;
                    fill_rect(dc, {s(24), y - s(10), rc.right - s(24), y - s(9)}, p.border);
                    const RECT chip{s(24), y + s(4), s(24) + s(12), y + s(16)};
                    fill_round_rect(dc, chip, s(3), p.badge_fg[section.badge]);
                    const SIZE ns = measure_text(dc, section.name, f.heading);
                    draw_text(dc, section.name, {s(44), y, s(44) + ns.cx + 1, y + s(20)}, p.text, f.heading, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                    draw_text(dc, section.count, {s(44) + ns.cx + s(10), y, rc.right - s(24), y + s(20)}, p.text2, f.ui_small,
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                    draw_text(dc, section.list, {s(24), y + s(30), rc.right - s(24), y + s(30) + section.list_height}, p.text2, f.ui,
                              DT_LEFT | DT_WORDBREAK);
                }
            }

            LRESULT handle(const UINT msg, const WPARAM wp, const LPARAM lp) {
                switch (msg) {
                    case WM_SIZE:
                        measure();
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    case WM_GETMINMAXINFO: {
                        auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
                        mm->ptMinTrackSize = {s(360), s(300)};
                        return 0;
                    }
                    case WM_ERASEBKGND:
                        return 1;
                    case WM_PAINT: {
                        PAINTSTRUCT ps;
                        HDC dc = BeginPaint(hwnd, &ps);
                        RECT rc;
                        GetClientRect(hwnd, &rc);
                        HDC mem = CreateCompatibleDC(dc);
                        HBITMAP bmp = CreateCompatibleBitmap(dc, std::max(1L, rc.right), std::max(1L, rc.bottom));
                        auto old = SelectObject(mem, bmp);
                        paint(mem);
                        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
                        SelectObject(mem, old);
                        DeleteObject(bmp);
                        DeleteDC(mem);
                        EndPaint(hwnd, &ps);
                        return 0;
                    }
                    case WM_VSCROLL: {
                        RECT rc;
                        GetClientRect(hwnd, &rc);
                        SCROLLINFO si{};
                        si.cbSize = sizeof(si);
                        si.fMask = SIF_ALL;
                        GetScrollInfo(hwnd, SB_VERT, &si);
                        switch (LOWORD(wp)) {
                            case SB_LINEUP: scroll_to(pos - s(40)); break;
                            case SB_LINEDOWN: scroll_to(pos + s(40)); break;
                            case SB_PAGEUP: scroll_to(pos - static_cast<int>(rc.bottom)); break;
                            case SB_PAGEDOWN: scroll_to(pos + static_cast<int>(rc.bottom)); break;
                            case SB_THUMBTRACK:
                            case SB_THUMBPOSITION: scroll_to(si.nTrackPos); break;
                            case SB_TOP: scroll_to(0); break;
                            case SB_BOTTOM: scroll_to(content_height); break;
                            default: break;
                        }
                        return 0;
                    }
                    case WM_MOUSEWHEEL:
                        scroll_to(pos - GET_WHEEL_DELTA_WPARAM(wp) * s(48) / WHEEL_DELTA);
                        return 0;
                    case WM_GETDLGCODE:
                        return DLGC_WANTALLKEYS;
                    case WM_KEYDOWN: {
                        RECT rc;
                        GetClientRect(hwnd, &rc);
                        switch (wp) {
                            case VK_UP: scroll_to(pos - s(40)); break;
                            case VK_DOWN: scroll_to(pos + s(40)); break;
                            case VK_PRIOR: scroll_to(pos - static_cast<int>(rc.bottom)); break;
                            case VK_NEXT: scroll_to(pos + static_cast<int>(rc.bottom)); break;
                            case VK_HOME: scroll_to(0); break;
                            case VK_END: scroll_to(content_height); break;
                            default: break;
                        }
                        return 0;
                    }
                    case WM_DPICHANGED: {
                        dpi = HIWORD(wp);
                        const auto* r = reinterpret_cast<RECT*>(lp);
                        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
                        measure();
                        return 0;
                    }
                    case WM_COMMAND:
                        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) done = true;
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
                auto* self = self_of<FormatsWindow>(hwnd, msg, lp);
                return self ? self->handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
            }
        };
    } // namespace

    void show_about_window(HWND owner, HINSTANCE instance, const std::vector<std::string>& extensions) {
        register_class(instance, L"ChiselAbout", AboutWindow::proc);
        AboutWindow w;
        w.instance = instance;
        w.extensions = &extensions;
        w.dpi = GetDpiForWindow(owner);
        create_centered(owner, instance, L"ChiselAbout", tr(L"About Chisel"), WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, kAboutW,
                        kAboutH, w.dpi, &w);
        if (!w.hwnd) return;
        const auto link = [&](const std::wstring& text, const int id) {
            return CreateWindowExW(0, WC_BUTTONW, text.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0, w.hwnd,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
        };
        w.link = link(L"github.com/Snesnopic/chisel", ID_LINK);
        w.credit = link(L"Mahary Esposito", ID_CREDIT);
        w.formats = link(trf(L"Supported formats ({0})", {std::to_wstring(extensions.size())}), ID_FORMATS);
        w.ok = CreateWindowExW(0, WC_BUTTONW, tr(L"OK"), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0, w.hwnd,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)), instance, nullptr);
        apply_window_theme(w.hwnd);
        w.layout();
        SetFocus(w.ok);
        run_modal(owner, w.hwnd, w.done);
        if (w.icon) DestroyIcon(w.icon);
    }

    void show_formats_window(HWND owner, HINSTANCE instance, const std::vector<std::string>& extensions) {
        register_class(instance, L"ChiselFormats", FormatsWindow::proc);
        FormatsWindow w;
        w.dpi = GetDpiForWindow(owner);
        const auto groups = chisel_app::group_by_kind(extensions);
        for (const auto& [kind, list] : groups) {
            FormatsWindow::Section section;
            section.name = kind_text(kind);
            section.count = trn(L"{0} format", L"{0} formats", list.size());
            section.badge = list.empty() ? CatApp : category_for(list.front());
            for (const auto& ext : list) {
                if (!section.list.empty()) section.list += L",  ";
                section.list += widen(ext);
            }
            w.sections.push_back(std::move(section));
        }
        w.summary = trn(L"{0} extension in {1}", L"{0} extensions in {1}", extensions.size(),
                        {trn(L"{0} category", L"{0} categories", groups.size())});
        create_centered(owner, instance, L"ChiselFormats", tr(L"Supported formats"),
                        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_VSCROLL, 600, 640, w.dpi, &w);
        if (!w.hwnd) return;
        apply_window_theme(w.hwnd);
        apply_control_theme(w.hwnd);
        w.measure();
        run_modal(owner, w.hwnd, w.done);
    }

} // namespace chisel_win
