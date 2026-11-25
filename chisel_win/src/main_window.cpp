#include "main_window.hpp"
#include "i18n.hpp"
#include "info_windows.hpp"
#include "platform.hpp"
#include "settings_window.hpp"
#include "stats_window.hpp"
#include "ui.hpp"

#include "chisel_app/paths.hpp"

#include <shellapi.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <ctime>
#include <fstream>

#ifndef CHISEL_VERSION
#define CHISEL_VERSION "dev"
#endif

namespace fs = std::filesystem;
using chisel_app::Row;
using chisel_app::Status;

namespace chisel_win {

    namespace {
        constexpr UINT WM_APP_CHANGED = WM_APP + 1;
        constexpr UINT WM_APP_ADDED = WM_APP + 2;

        enum : int {
            IDC_LIST = 100, IDC_LOG, IDC_OPEN, IDC_COPY, IDC_EMPTY_FILES, IDC_EMPTY_FOLDER, IDC_FORMATS, IDC_INFO,
            CMD_ADD_FILES = 200, CMD_ADD_FOLDER, CMD_START, CMD_DRY_RUN, CMD_STOP, CMD_CLEAR, CMD_CLEAR_ALL,
            CMD_CLEAR_FINISHED, CMD_STATS, CMD_SETTINGS, CMD_ABOUT, CMD_REMOVE
        };

        constexpr int kToolbarH = 52, kStatusH = 30, kInspectorW = 320, kInfoH = 40;
        constexpr int kColOriginal = 84, kColNew = 84, kColSaved = 150, kColStatus = 140;

        struct StatusLook {
            std::wstring label;
            COLORREF dot;
            COLORREF color;
        };

        std::wstring skip_short(const chisel_app::SkipKind kind) {
            using chisel_app::SkipKind;
            switch (kind) {
                case SkipKind::Signed: return tr(L"signed");
                case SkipKind::Unsupported: return tr(L"unsupported");
                case SkipKind::Empty: return tr(L"empty");
                case SkipKind::Junk: return tr(L"junk");
                case SkipKind::NothingToDo: return tr(L"nothing to do");
                case SkipKind::NestingTooDeep: return tr(L"too deep");
                case SkipKind::ReadOnly: return tr(L"read-only");
                case SkipKind::ChangedDuringRun: return tr(L"changed");
                default: return L"";
            }
        }

        StatusLook status_look(const Row& row) {
            const auto& p = palette();
            switch (row.status) {
                case Status::Completed: return {tr(L"Completed"), p.ok, p.text};
                case Status::Simulated: return {tr(L"Would be smaller"), p.ok, p.text2};
                case Status::Processing: return {tr(L"Processing…"), p.accent, p.text};
                case Status::NoGain: return {tr(L"No gain"), p.text3, p.text2};
                case Status::Skipped: {
                    const auto why = skip_short(row.skip);
                    return {why.empty() ? std::wstring(tr(L"Skipped")) : trf(L"Skipped: {0}", {why}), p.warn, p.text2};
                }
                case Status::Error: return {tr(L"Error"), p.err, p.err};
                case Status::Stopped: return {tr(L"Stopped"), p.text3, p.text2};
                case Status::Pending: break;
            }
            return {tr(L"Pending"), p.pending_dot, p.text2};
        }

        bool has_gain(const Row& row) {
            return (row.status == Status::Completed || row.status == Status::Simulated) && row.size_after && *row.size_after < row.size;
        }

        double gain_percent(const Row& row) {
            if (!has_gain(row) || row.size == 0) return 0;
            return 100.0 * static_cast<double>(row.size - *row.size_after) / static_cast<double>(row.size);
        }

        std::wstring mode_label(const chisel_app::Settings& s) {
            switch (s.output_mode) {
                case chisel_app::OutputMode::Overwrite: return tr(L"Replace originals");
                case chisel_app::OutputMode::KeepOriginal: return tr(L"Keep originals");
                case chisel_app::OutputMode::SideBySide: return tr(L"Save next to originals");
                case chisel_app::OutputMode::OutputFolder:
                    return s.output_folder.empty() ? std::wstring(tr(L"Save to a folder"))
                                                   : trf(L"Save to {0}", {s.output_folder.filename().wstring()});
            }
            return L"";
        }

        std::wstring reason_text(const Row& row) {
            if (row.status == Status::Error) return widen(row.error);
            if (row.status != Status::Skipped && row.status != Status::NoGain) return L"";
            switch (row.skip) {
                case chisel_app::SkipKind::Signed: return tr(L"Digitally signed: left untouched so its signature stays valid.");
                case chisel_app::SkipKind::Unsupported: return tr(L"No processor handles this format.");
                case chisel_app::SkipKind::ReadOnly: return tr(L"Read-only: it, or its folder, can't be written.");
                case chisel_app::SkipKind::ChangedDuringRun: return tr(L"Something else changed it while Chisel was working on it: left as it was.");
                case chisel_app::SkipKind::Empty: return tr(L"Empty file.");
                case chisel_app::SkipKind::NothingToDo: return tr(L"Nothing to recompress in it.");
                default: break;
            }
            if (row.status == Status::Skipped) {
                // a container's result settles what was inside it
                return row.skip_message.empty() ? std::wstring(tr(L"Discarded with its container.")) : widen(row.skip_message);
            }
            return L"";
        }

        std::wstring note_text(const chisel_app::SkipNote note) {
            switch (note) {
                case chisel_app::SkipNote::ReadOnly: return tr(L"Read-only");
                case chisel_app::SkipNote::ChangedDuringRun: return tr(L"Changed during the run");
                case chisel_app::SkipNote::None: break;
            }
            return L"";
        }

        std::wstring start_error_text(const chisel_app::StartError error) {
            using chisel_app::StartError;
            switch (error) {
                case StartError::AlreadyRunning: return tr(L"a run is already going");
                case StartError::NothingPending: return tr(L"nothing is waiting to be processed");
                case StartError::NoOutputFolder: return tr(L"no output folder is set");
                case StartError::OutputFolderUnavailable: return tr(L"the output folder can't be created");
                case StartError::OutputFolderIsInputFolder: return tr(L"the output folder is the folder the files come from");
                case StartError::None: break;
            }
            return widen(chisel_app::to_string(error));
        }
    } // namespace

    MainWindow::MainWindow(HINSTANCE instance, chisel_app::Session& session, chisel_app::KeyValueStore& store,
                           chisel_app::StatsStore& stats)
        : instance_(instance), session_(session), store_(store), stats_(stats), settings_(chisel_app::load_settings(store)) {}

    MainWindow::~MainWindow() {
        session_.set_on_changed({});
        if (add_thread_.joinable()) add_thread_.join();
        if (taskbar_) taskbar_->Release();
        if (row_height_) ImageList_Destroy(row_height_);
        if (panel_brush_) DeleteObject(panel_brush_);
        if (log_brush_) DeleteObject(log_brush_);
        if (accel_) DestroyAcceleratorTable(accel_);
    }

    int MainWindow::s(const int px) const { return scale(px, dpi_); }

    bool MainWindow::create(const int show) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = proc;
        wc.hInstance = instance_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(1));
        wc.hIconSm = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                                   GetSystemMetrics(SM_CYSMICON), 0));
        wc.lpszClassName = L"ChiselMainWindow";
        RegisterClassExW(&wc);

        ACCEL accels[] = {
            {FCONTROL | FVIRTKEY, 'O', CMD_ADD_FILES},
            {FCONTROL | FSHIFT | FVIRTKEY, 'O', CMD_ADD_FOLDER},
            {FVIRTKEY, VK_F5, CMD_START},
            {FSHIFT | FVIRTKEY, VK_F5, CMD_STOP},
            {FCONTROL | FVIRTKEY, VK_OEM_COMMA, CMD_SETTINGS},
        };
        accel_ = CreateAcceleratorTableW(accels, static_cast<int>(std::size(accels)));
        taskbar_created_ = RegisterWindowMessageW(L"TaskbarButtonCreated");

        const UINT system_dpi = GetDpiForSystem();
        hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"Chisel", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                                CW_USEDEFAULT, scale(1280, system_dpi), scale(800, system_dpi), nullptr, nullptr,
                                instance_, this);
        if (!hwnd_) return false;
        ShowWindow(hwnd_, show);
        UpdateWindow(hwnd_);
        return true;
    }

    bool MainWindow::pre_translate(MSG& msg) {
        if (stats_window_ && stats_window_->hwnd() && (msg.hwnd == stats_window_->hwnd() || IsChild(stats_window_->hwnd(), msg.hwnd))) {
            return IsDialogMessageW(stats_window_->hwnd(), &msg) != FALSE;
        }
        if (accel_ && TranslateAcceleratorW(hwnd_, accel_, &msg)) return true;
        return IsDialogMessageW(hwnd_, &msg) != FALSE;
    }

    LRESULT CALLBACK MainWindow::proc(HWND hwnd, const UINT msg, const WPARAM wp, const LPARAM lp) {
        MainWindow* self = nullptr;
        if (msg == WM_NCCREATE) {
            self = static_cast<MainWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        return self ? self->handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
    }

    LRESULT CALLBACK MainWindow::list_proc(HWND hwnd, const UINT msg, const WPARAM wp, const LPARAM lp, UINT_PTR, const DWORD_PTR self) {
        if (msg == WM_NOTIFY) {
            auto* hdr = reinterpret_cast<NMHDR*>(lp);
            if (hdr->hwndFrom == ListView_GetHeader(hwnd) && hdr->code == NM_CUSTOMDRAW) {
                return reinterpret_cast<MainWindow*>(self)->draw_header_item(reinterpret_cast<NMCUSTOMDRAW*>(lp));
            }
        } else if (msg == WM_NCDESTROY) {
            RemoveWindowSubclass(hwnd, list_proc, 1);
        }
        return DefSubclassProc(hwnd, msg, wp, lp);
    }

    void MainWindow::create_children() {
        list_ = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA |
                                LVS_SINGLESEL | LVS_SHOWSELALWAYS, 0, 0, 0, 0, hwnd_,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LIST)), instance_, nullptr);
        ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        SetWindowSubclass(list_, list_proc, 1, reinterpret_cast<DWORD_PTR>(this));
        const wchar_t* names[] = {tr(L"Name"), tr(L"Original"), tr(L"New size"), tr(L"Saved"), tr(L"Status")};
        const int formats[] = {LVCFMT_LEFT, LVCFMT_RIGHT, LVCFMT_RIGHT, LVCFMT_LEFT, LVCFMT_LEFT};
        for (int i = 0; i < 5; ++i) {
            LVCOLUMNW col{};
            col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
            col.fmt = formats[i];
            col.pszText = const_cast<wchar_t*>(names[i]);
            col.cx = 100;
            ListView_InsertColumn(list_, i, &col);
        }
        log_ = CreateWindowExW(0, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | ES_MULTILINE | ES_READONLY |
                               ES_AUTOVSCROLL, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LOG)),
                               instance_, nullptr);
        const auto button = [&](const wchar_t* text, const int id) {
            return CreateWindowExW(0, WC_BUTTONW, text, WS_CHILD | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0, hwnd_,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        };
        open_button_ = button(tr(L"Open file location"), IDC_OPEN);
        copy_button_ = button(tr(L"Copy log"), IDC_COPY);
        empty_files_ = button(tr(L"Add files"), IDC_EMPTY_FILES);
        empty_folder_ = button(tr(L"Add folder"), IDC_EMPTY_FOLDER);
        formats_link_ = button(tr(L"See the supported formats"), IDC_FORMATS);
        info_button_ = button(tr(L"Show"), IDC_INFO);
        DragAcceptFiles(hwnd_, TRUE);

        tools_.clear();
        const auto add = [&](ToolItem item) { tools_.push_back(std::move(item)); };
        add({.kind = ToolItem::Button, .command = CMD_ADD_FILES, .glyph = glyph::add_file, .text = tr(L"Add files")});
        add({.kind = ToolItem::Button, .command = CMD_ADD_FOLDER, .glyph = glyph::add_folder, .text = tr(L"Add folder")});
        add({.kind = ToolItem::Separator});
        add({.kind = ToolItem::Accent, .command = CMD_START, .glyph = glyph::play, .text = tr(L"Start"), .split = true});
        add({.kind = ToolItem::Button, .command = CMD_STOP, .glyph = glyph::stop, .text = tr(L"Stop")});
        add({.kind = ToolItem::Separator});
        add({.kind = ToolItem::Button, .command = CMD_CLEAR, .glyph = glyph::trash, .text = tr(L"Clear"), .chevron = true});
        add({.kind = ToolItem::Spacer});
        add({.kind = ToolItem::Button, .command = CMD_STATS, .glyph = glyph::chart, .text = tr(L"Statistics"), .right = true});
        add({.kind = ToolItem::Button, .command = CMD_SETTINGS, .glyph = glyph::settings, .text = tr(L"Settings"), .right = true});
        add({.kind = ToolItem::Button, .command = CMD_ABOUT, .glyph = glyph::info, .text = tr(L"About Chisel"), .icon_only = true, .right = true});
    }

    void MainWindow::apply_fonts() {
        const auto& f = fonts(dpi_);
        SendMessageW(list_, WM_SETFONT, reinterpret_cast<WPARAM>(f.ui), TRUE);
        SendMessageW(log_, WM_SETFONT, reinterpret_cast<WPARAM>(f.mono), TRUE);
        // the list's row height comes from its small image list
        if (row_height_) ImageList_Destroy(row_height_);
        row_height_ = ImageList_Create(1, s(34), ILC_COLOR32, 1, 1);
        ListView_SetImageList(list_, row_height_, LVSIL_SMALL);
    }

    void MainWindow::apply_theme() {
        const auto& p = palette();
        apply_window_theme(hwnd_);
        apply_control_theme(list_);
        HWND header = ListView_GetHeader(list_);
        SetWindowTheme(header, is_dark() ? L"DarkMode_ItemsView" : L"ItemsView", nullptr);
        InvalidateRect(header, nullptr, TRUE);
        apply_control_theme(log_);
        ListView_SetBkColor(list_, p.surface);
        ListView_SetTextBkColor(list_, p.surface);
        ListView_SetTextColor(list_, p.text);
        if (panel_brush_) DeleteObject(panel_brush_);
        if (log_brush_) DeleteObject(log_brush_);
        panel_brush_ = CreateSolidBrush(p.panel);
        log_brush_ = CreateSolidBrush(p.log_bg);
        RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
        if (stats_window_) stats_window_->theme_changed();
    }

    void MainWindow::layout_toolbar(const int width) {
        HDC dc = GetDC(hwnd_);
        const auto& f = fonts(dpi_);
        const int h = s(36);
        const int top = (s(kToolbarH) - h) / 2;
        const auto item_width = [&](const ToolItem& t) {
            if (t.kind == ToolItem::Separator) return s(13);
            if (t.icon_only) return s(36);
            const int text = measure_text(dc, t.text, t.kind == ToolItem::Accent ? f.ui_semibold : f.ui).cx;
            int w = s(12) + s(16) + s(8) + text + s(12);
            if (t.split) w += s(1) + s(30);
            if (t.chevron) w += s(8) + s(10);
            return w;
        };
        int x = s(12);
        int right_total = 0;
        for (const auto& t : tools_) {
            if (t.right) right_total += item_width(t) + s(4);
        }
        int rx = width - s(12) - right_total;
        // translated labels can make the toolbar wider than the design's minimum window
        int left_total = 0;
        for (const auto& t : tools_) {
            if (!t.right && t.kind != ToolItem::Spacer) left_total += item_width(t) + s(4);
        }
        toolbar_min_w_ = s(12) + left_total + s(24) + right_total + s(12);
        for (auto& t : tools_) {
            if (t.kind == ToolItem::Spacer) continue;
            const int w = item_width(t);
            int& cursor = t.right ? rx : x;
            t.rc = {cursor, top, cursor + w, top + h};
            t.drop = {};
            if (t.split) t.drop = {t.rc.right - s(30), top, t.rc.right, top + h};
            cursor += w + s(4);
        }
        ReleaseDC(hwnd_, dc);
    }

    void MainWindow::layout() {
        RECT rc;
        GetClientRect(hwnd_, &rc);
        toolbar_rc_ = {0, 0, rc.right, s(kToolbarH)};
        status_rc_ = {0, rc.bottom - s(kStatusH), rc.right, rc.bottom};
        content_rc_ = {0, toolbar_rc_.bottom, rc.right, status_rc_.top};
        layout_toolbar(rc.right);
        const bool empty = snap_.rows.empty();
        const int inspector_w = empty ? 0 : s(kInspectorW);
        inspector_rc_ = {content_rc_.right - inspector_w, content_rc_.top, content_rc_.right, content_rc_.bottom};
        RECT left{content_rc_.left, content_rc_.top, inspector_rc_.left - (empty ? 0 : 1), content_rc_.bottom};
        info_rc_ = {};
        if (info_kind_ != InfoKind::None) {
            info_rc_ = {left.left + s(12), left.top + s(12), left.right - s(12), left.top + s(12) + s(kInfoH)};
            if (!empty) left.top = info_rc_.bottom + s(4);
        }
        list_rc_ = left;

        const auto place = [](HWND w, const RECT& r, const bool visible) {
            SetWindowPos(w, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                         SWP_NOZORDER | SWP_NOACTIVATE | (visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
        };
        place(list_, list_rc_, !empty);
        if (!empty) {
            // columns are at least as wide as their translated titles; status also fits its longer labels
            HDC dc = GetDC(hwnd_);
            const auto& f = fonts(dpi_);
            const auto fit = [&](const int base, const std::wstring& text, HFONT font, const int pad) {
                return std::max(s(base), static_cast<int>(measure_text(dc, text, font).cx) + pad);
            };
            const int original = fit(kColOriginal, tr(L"Original"), f.ui_small_semibold, s(16));
            const int after = fit(kColNew, tr(L"New size"), f.ui_small_semibold, s(16));
            const int saved = fit(kColSaved, tr(L"Saved"), f.ui_small_semibold, s(36));
            int status = fit(kColStatus, tr(L"Status"), f.ui_small_semibold, s(16));
            status = std::max(status, std::min(s(190), fit(0, tr(L"Would be smaller"), f.ui, s(24))));
            status = std::max(status, std::min(s(190), fit(0, trf(L"Skipped: {0}", {tr(L"unsupported")}), f.ui, s(24))));
            ReleaseDC(hwnd_, dc);
            const int scroll = GetSystemMetricsForDpi(SM_CXVSCROLL, dpi_);
            const int fixed = original + after + saved + status;
            const int name = std::max(s(200), static_cast<int>(list_rc_.right - list_rc_.left) - fixed - scroll - 2);
            ListView_SetColumnWidth(list_, 0, name);
            ListView_SetColumnWidth(list_, 1, original);
            ListView_SetColumnWidth(list_, 2, after);
            ListView_SetColumnWidth(list_, 3, saved);
            ListView_SetColumnWidth(list_, 4, status);
        }
        const int ix = inspector_rc_.left;
        const int iy = inspector_rc_.top;
        const int open_w = std::max(s(150), button_width(open_button_, ButtonStyle::Normal, true, dpi_));
        const int copy_w = std::max(s(100), button_width(copy_button_, ButtonStyle::Normal, true, dpi_));
        place(open_button_, {ix + s(20), iy + s(96), ix + s(20) + open_w, iy + s(126)}, !empty);
        place(copy_button_, {ix + s(20) + open_w + s(8), iy + s(96), ix + s(20) + open_w + s(8) + copy_w, iy + s(126)}, !empty);
        const RECT log_box{ix + s(20), iy + s(383), inspector_rc_.right - s(20), inspector_rc_.bottom - s(20)};
        place(log_, {log_box.left + s(10), log_box.top + s(8), log_box.right - s(4), log_box.bottom - s(4)},
              !empty && log_box.bottom - log_box.top > s(40));
        const int info_w = std::max(s(64), button_width(info_button_, ButtonStyle::Normal, false, dpi_));
        place(info_button_, {info_rc_.right - s(12) - info_w, info_rc_.top + s(6), info_rc_.right - s(12), info_rc_.bottom - s(6)},
              info_kind_ != InfoKind::None && info_action_);

        // the empty state's box is centered in the content area
        const int cx = (content_rc_.left + content_rc_.right) / 2;
        const int cy = (content_rc_.top + content_rc_.bottom) / 2;
        const int buttons_y = cy + s(40);
        const int files_w = std::max(s(110), button_width(empty_files_, ButtonStyle::Accent, true, dpi_));
        const int folder_w = std::max(s(110), button_width(empty_folder_, ButtonStyle::Normal, true, dpi_));
        const int formats_w = std::max(s(220), button_width(formats_link_, ButtonStyle::Link, false, dpi_));
        place(empty_files_, {cx - s(4) - files_w, buttons_y, cx - s(4), buttons_y + s(36)}, empty);
        place(empty_folder_, {cx + s(4), buttons_y, cx + s(4) + folder_w, buttons_y + s(36)}, empty);
        place(formats_link_, {cx - formats_w / 2, buttons_y + s(42), cx + formats_w / 2, buttons_y + s(42) + s(24)}, empty);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    LRESULT MainWindow::handle(const UINT msg, const WPARAM wp, const LPARAM lp) {
        if (msg == taskbar_created_ && taskbar_created_ != 0) {
            if (!taskbar_ && SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&taskbar_)))) {
                taskbar_->HrInit();
            }
            update_taskbar();
            return 0;
        }
        switch (msg) {
            case WM_CREATE: {
                dpi_ = GetDpiForWindow(hwnd_);
                create_children();
                apply_fonts();
                apply_theme();
                session_.set_on_changed([hwnd = hwnd_] { PostMessageW(hwnd, WM_APP_CHANGED, 0, 0); });
                on_changed();
                return 0;
            }
            case WM_SIZE:
                layout();
                return 0;
            case WM_GETMINMAXINFO: {
                auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
                RECT frame{0, 0, toolbar_min_w_, 0};
                AdjustWindowRectExForDpi(&frame, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi_);
                mm->ptMinTrackSize = {std::max(s(860), static_cast<int>(frame.right - frame.left)), s(520)};
                return 0;
            }
            case WM_DPICHANGED: {
                dpi_ = HIWORD(wp);
                apply_fonts();
                const auto* r = reinterpret_cast<RECT*>(lp);
                SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
                layout();
                return 0;
            }
            case WM_SETTINGCHANGE:
                if (lp && wcscmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0 && refresh_theme()) apply_theme();
                break;
            case WM_ERASEBKGND:
                return 1;
            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC dc = BeginPaint(hwnd_, &ps);
                RECT rc;
                GetClientRect(hwnd_, &rc);
                HDC mem = CreateCompatibleDC(dc);
                HBITMAP bmp = CreateCompatibleBitmap(dc, std::max(1L, rc.right), std::max(1L, rc.bottom));
                auto old = SelectObject(mem, bmp);
                paint(mem);
                BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left,
                       ps.rcPaint.bottom - ps.rcPaint.top, mem, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
                SelectObject(mem, old);
                DeleteObject(bmp);
                DeleteDC(mem);
                EndPaint(hwnd_, &ps);
                return 0;
            }
            case WM_MOUSEMOVE: {
                const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                const int hot = hit_tool(pt, nullptr);
                if (hot != hot_tool_) {
                    hot_tool_ = hot;
                    InvalidateRect(hwnd_, &toolbar_rc_, FALSE);
                }
                if (!tracking_mouse_) {
                    TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
                    tracking_mouse_ = TrackMouseEvent(&tme) != FALSE;
                }
                return 0;
            }
            case WM_MOUSELEAVE:
                tracking_mouse_ = false;
                if (hot_tool_ != -1) {
                    hot_tool_ = -1;
                    InvalidateRect(hwnd_, &toolbar_rc_, FALSE);
                }
                return 0;
            case WM_LBUTTONDOWN: {
                const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                pressed_tool_ = hit_tool(pt, nullptr);
                if (pressed_tool_ >= 0) {
                    SetCapture(hwnd_);
                    InvalidateRect(hwnd_, &toolbar_rc_, FALSE);
                }
                return 0;
            }
            case WM_LBUTTONUP: {
                const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                bool in_drop = false;
                const int hit = hit_tool(pt, &in_drop);
                const int pressed = pressed_tool_;
                pressed_tool_ = -1;
                if (GetCapture() == hwnd_) ReleaseCapture();
                InvalidateRect(hwnd_, &toolbar_rc_, FALSE);
                if (pressed >= 0 && pressed == hit && tools_[hit].enabled) {
                    const auto& t = tools_[hit];
                    if (t.split && in_drop) show_menu(t, true);
                    else if (t.chevron) show_menu(t, false);
                    else on_command(t.command);
                }
                return 0;
            }
            case WM_COMMAND:
                on_command(LOWORD(wp));
                return 0;
            case WM_NOTIFY:
                return on_notify(reinterpret_cast<NMHDR*>(lp));
            case WM_DRAWITEM: {
                const auto* di = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
                switch (di->CtlID) {
                    case IDC_EMPTY_FILES: draw_button(*di, ButtonStyle::Accent, glyph::add_file, dpi_); break;
                    case IDC_EMPTY_FOLDER: draw_button(*di, ButtonStyle::Normal, glyph::add_folder, dpi_); break;
                    case IDC_FORMATS: draw_button(*di, ButtonStyle::Link, 0, dpi_); break;
                    case IDC_OPEN: draw_button(*di, ButtonStyle::Normal, glyph::folder_open, dpi_); break;
                    case IDC_COPY: draw_button(*di, ButtonStyle::Normal, glyph::copy, dpi_); break;
                    default: draw_button(*di, ButtonStyle::Normal, 0, dpi_); break;
                }
                return TRUE;
            }
            case WM_CTLCOLORBTN: {
                HDC dc = reinterpret_cast<HDC>(wp);
                const COLORREF color = region_color(reinterpret_cast<HWND>(lp));
                SetBkColor(dc, color);
                SetDCBrushColor(dc, color);
                return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
            }
            case WM_CTLCOLORSTATIC:
            case WM_CTLCOLOREDIT: {
                HDC dc = reinterpret_cast<HDC>(wp);
                if (reinterpret_cast<HWND>(lp) == log_) {
                    SetTextColor(dc, palette().text);
                    SetBkColor(dc, palette().log_bg);
                    return reinterpret_cast<LRESULT>(log_brush_);
                }
                SetTextColor(dc, palette().text);
                SetBkColor(dc, palette().panel);
                return reinterpret_cast<LRESULT>(panel_brush_);
            }
            case WM_DROPFILES: {
                auto drop = reinterpret_cast<HDROP>(wp);
                std::vector<fs::path> paths;
                const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
                for (UINT i = 0; i < count; ++i) {
                    const UINT n = DragQueryFileW(drop, i, nullptr, 0);
                    std::wstring path(n + 1, L'\0');
                    DragQueryFileW(drop, i, path.data(), n + 1);
                    path.resize(n);
                    paths.emplace_back(path);
                }
                DragFinish(drop);
                add_paths(std::move(paths));
                return 0;
            }
            case WM_APP_CHANGED:
                on_changed();
                return 0;
            case WM_APP_ADDED: {
                std::unique_ptr<chisel_app::Session::AddResult> result(reinterpret_cast<chisel_app::Session::AddResult*>(lp));
                if (result && result->unsupported > 0) {
                    show_info(InfoKind::Notice, tr(L"Not added."),
                              trn(L"{0} file can't be made smaller by Chisel and wasn't added.",
                                  L"{0} files can't be made smaller by Chisel and weren't added.", result->unsupported),
                              true);
                } else if (result && result->added == 0 && result->requeued == 0 && result->duplicates > 0) {
                    show_info(InfoKind::Notice, tr(L"Already in the list."), tr(L"Those files are waiting to be processed."), false);
                }
                update_toolbar_state();
                InvalidateRect(hwnd_, &status_rc_, FALSE);
                return 0;
            }
            case WM_CLOSE:
                if (snap_.stats.running) {
                    if (MessageBoxW(hwnd_, tr(L"Files are being processed. Stop and quit? Files not finished yet are left as they are."),
                                    L"Chisel", MB_YESNO | MB_ICONQUESTION) != IDYES) {
                        return 0;
                    }
                    close_after_stop_ = true;
                    session_.stop();
                    return 0;
                }
                DestroyWindow(hwnd_);
                return 0;
            case WM_DESTROY:
                session_.set_on_changed({});
                PostQuitMessage(0);
                return 0;
            default:
                break;
        }
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }

    COLORREF MainWindow::region_color(HWND control) const {
        const auto& p = palette();
        if (control == open_button_ || control == copy_button_) return p.panel;
        if (control == info_button_) return info_kind_ == InfoKind::Errors ? p.err_bg : p.info_bg;
        if (control == empty_files_ || control == empty_folder_ || control == formats_link_) return p.surface;
        return p.surface;
    }

    int MainWindow::hit_tool(const POINT pt, bool* in_drop) const {
        for (int i = 0; i < static_cast<int>(tools_.size()); ++i) {
            const auto& t = tools_[static_cast<std::size_t>(i)];
            if (t.kind == ToolItem::Separator || t.kind == ToolItem::Spacer) continue;
            if (PtInRect(&t.rc, pt)) {
                if (in_drop) *in_drop = t.split && PtInRect(&t.drop, pt);
                return i;
            }
        }
        return -1;
    }

    void MainWindow::paint(HDC dc) {
        const auto& p = palette();
        RECT rc;
        GetClientRect(hwnd_, &rc);
        fill_rect(dc, rc, p.window);
        paint_toolbar(dc);
        if (snap_.rows.empty()) {
            paint_empty(dc);
            paint_info(dc);
        } else {
            fill_rect(dc, {content_rc_.left, content_rc_.top, inspector_rc_.left, content_rc_.bottom}, p.surface);
            fill_rect(dc, {inspector_rc_.left - 1, content_rc_.top, inspector_rc_.left, content_rc_.bottom}, p.border);
            paint_info(dc);
            paint_inspector(dc);
        }
        paint_status(dc);
    }

    void MainWindow::paint_toolbar(HDC dc) {
        const auto& p = palette();
        const auto& f = fonts(dpi_);
        fill_rect(dc, toolbar_rc_, p.window);
        fill_rect(dc, {toolbar_rc_.left, toolbar_rc_.bottom - 1, toolbar_rc_.right, toolbar_rc_.bottom}, p.border);
        for (int i = 0; i < static_cast<int>(tools_.size()); ++i) {
            const auto& t = tools_[static_cast<std::size_t>(i)];
            if (t.kind == ToolItem::Spacer) continue;
            if (t.kind == ToolItem::Separator) {
                const int x = t.rc.left + s(6);
                const int cy = (t.rc.top + t.rc.bottom) / 2;
                fill_rect(dc, {x, cy - s(10), x + 1, cy + s(10)}, p.border);
                continue;
            }
            const bool hot = i == hot_tool_ && t.enabled;
            const bool pressed = i == pressed_tool_ && t.enabled;
            COLORREF fg = t.enabled ? p.text : p.disabled_fg;
            if (t.kind == ToolItem::Accent) {
                COLORREF bg = t.enabled ? p.accent : p.disabled_bg;
                if (pressed) bg = RGB(GetRValue(bg) * 85 / 100, GetGValue(bg) * 85 / 100, GetBValue(bg) * 85 / 100);
                fill_round_rect(dc, t.rc, s(4), bg);
                fg = t.enabled ? p.on_accent : p.disabled_fg;
                fill_rect(dc, {t.drop.left, t.rc.top + s(8), t.drop.left + 1, t.rc.bottom - s(8)}, fg);
                draw_glyph(dc, glyph::chevron_down, t.drop, fg, f.icons_small);
            } else if (hot || pressed) {
                fill_round_rect(dc, t.rc, s(4), p.hover);
            }
            if (t.icon_only) {
                draw_glyph(dc, t.glyph, t.rc, fg, f.icons);
                continue;
            }
            int x = t.rc.left + s(12);
            draw_glyph(dc, t.glyph, {x, t.rc.top, x + s(16), t.rc.bottom}, fg, f.icons);
            x += s(16) + s(8);
            HFONT font = t.kind == ToolItem::Accent ? f.ui_semibold : f.ui;
            const SIZE ts = measure_text(dc, t.text, font);
            draw_text(dc, t.text, {x, t.rc.top, x + ts.cx + 1, t.rc.bottom}, fg, font, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            x += ts.cx;
            if (t.chevron) {
                x += s(8);
                draw_glyph(dc, glyph::chevron_down, {x, t.rc.top, x + s(10), t.rc.bottom}, fg, f.icons_small);
            }
        }
    }

    void MainWindow::paint_info(HDC dc) {
        if (info_kind_ == InfoKind::None) return;
        const auto& p = palette();
        const auto& f = fonts(dpi_);
        const bool errors = info_kind_ == InfoKind::Errors;
        fill_round_rect(dc, info_rc_, s(6), errors ? p.err_bg : p.info_bg, errors ? p.err_line : p.info_line);
        int x = info_rc_.left + s(12);
        draw_glyph(dc, errors ? glyph::error : glyph::info, {x, info_rc_.top, x + s(16), info_rc_.bottom}, errors ? p.err : p.accent, f.icons);
        x += s(16) + s(10);
        const SIZE ts = measure_text(dc, info_title_, f.ui_semibold);
        draw_text(dc, info_title_, {x, info_rc_.top, x + ts.cx + 1, info_rc_.bottom}, p.text, f.ui_semibold,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        x += ts.cx + s(10);
        const int right = info_rc_.right - s(12) - (info_action_ ? s(64) + s(10) : 0);
        draw_text(dc, info_text_, {x, info_rc_.top, right, info_rc_.bottom}, p.text2, f.ui,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    void MainWindow::paint_inspector(HDC dc) {
        const auto& p = palette();
        const auto& f = fonts(dpi_);
        const RECT& r = inspector_rc_;
        fill_rect(dc, r, p.panel);
        const Row* row = selected_row();
        const int x = r.left;
        const int y = r.top;
        if (!row) {
            draw_text(dc, tr(L"Select a file to see its details."), {x + s(20), y, r.right - s(20), r.bottom}, p.text2, f.ui,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            return;
        }
        const auto cat = category_for(row->extension);
        const RECT badge{x + s(20), y + s(20), x + s(68), y + s(68)};
        fill_round_rect(dc, badge, s(8), p.badge_bg[cat]);
        draw_text(dc, badge_text(row->extension), badge, p.badge_fg[cat], f.badge_big, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        draw_text(dc, widen(row->name), {x + s(80), y + s(20), r.right - s(20), y + s(42)}, p.text, f.name,
                  DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        std::wstring where;
        if (row->parent != 0) {
            if (const Row* parent = row_by_id(row->parent)) where = trf(L"Inside {0}", {widen(parent->name)});
        } else {
            where = row->path.parent_path().wstring();
        }
        draw_text(dc, where, {x + s(80), y + s(46), r.right - s(20), y + s(66)}, p.text2, f.ui_small,
                  DT_LEFT | DT_SINGLELINE | DT_PATH_ELLIPSIS);
        fill_rect(dc, {x, y + s(84), r.right, y + s(84) + 1}, p.border);
        fill_rect(dc, {x, y + s(138), r.right, y + s(138) + 1}, p.border);

        const auto look = status_look(*row);
        const int lx = x + s(20);
        // the label column fits the longest translated label, leaving the values at least half the panel
        int label_w = s(104);
        for (const wchar_t* label : {tr(L"Status"), tr(L"Original size"), tr(L"New size"), tr(L"Saved"), tr(L"Error"), tr(L"Reason"),
                                     tr(L"Skipped")}) {
            label_w = std::max(label_w, static_cast<int>(measure_text(dc, label, f.ui).cx));
        }
        label_w = std::min(label_w, static_cast<int>(r.right - r.left) / 2 - s(32));
        const int vx = lx + label_w + s(12);
        const auto field = [&](const int fy, const wchar_t* label) {
            draw_text(dc, label, {lx, fy, vx, fy + s(20)}, p.text2, f.ui, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        };
        int fy = y + s(154);
        field(fy, tr(L"Status"));
        draw_dot(dc, vx + s(4), fy + s(10), s(8), look.dot);
        draw_text(dc, look.label, {vx + s(16), fy, r.right - s(20), fy + s(20)}, look.color, f.ui_semibold,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        fy += s(30);
        field(fy, tr(L"Original size"));
        draw_text(dc, format_bytes(row->size), {vx, fy, r.right - s(20), fy + s(20)}, p.text, f.ui, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        fy += s(30);
        field(fy, tr(L"New size"));
        std::wstring after = L"—";
        if (row->size_after && (row->status == Status::Completed || row->status == Status::Simulated || row->status == Status::NoGain)) {
            after = format_bytes(*row->size_after);
        }
        draw_text(dc, after, {vx, fy, r.right - s(20), fy + s(20)}, p.text, f.ui, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        fy += s(30);
        field(fy, tr(L"Saved"));
        std::wstring saved = L"—";
        COLORREF saved_color = p.text2;
        if (has_gain(*row)) {
            saved = format_bytes(row->size - *row->size_after) + L" (" + format_percent(gain_percent(*row)) + L")";
            saved_color = p.ok;
        } else if (row->status == Status::NoGain) {
            saved = tr(L"Nothing: original kept");
        }
        draw_text(dc, saved, {vx, fy, r.right - s(20), fy + s(20)}, saved_color, f.ui, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        fy += s(30);
        const std::wstring reason = reason_text(*row);
        if (!reason.empty()) {
            field(fy, row->status == Status::Error ? tr(L"Error") : tr(L"Reason"));
            draw_text(dc, reason, {vx, fy + s(2), r.right - s(20), fy + s(54)}, row->status == Status::Error ? p.err : p.text, f.ui,
                      DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS | DT_EDITCONTROL);
        } else if (row->note != chisel_app::SkipNote::None) {
            field(fy, tr(L"Skipped"));
            draw_text(dc, note_text(row->note), {vx, fy, r.right - s(20), fy + s(20)}, p.text, f.ui,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        fill_rect(dc, {x, y + s(341), r.right, y + s(341) + 1}, p.border);
        draw_text(dc, tr(L"Process log"), {lx, y + s(357), r.right - s(20), y + s(375)}, p.text2, f.ui_small_semibold,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        const RECT box{x + s(20), y + s(383), r.right - s(20), r.bottom - s(20)};
        if (box.bottom - box.top > s(40)) fill_round_rect(dc, box, s(6), p.log_bg, p.border);
    }

    void MainWindow::paint_status(HDC dc) {
        const auto& p = palette();
        const auto& f = fonts(dpi_);
        const auto& st = snap_.stats;
        fill_rect(dc, status_rc_, p.window);
        fill_rect(dc, {status_rc_.left, status_rc_.top, status_rc_.right, status_rc_.top + 1}, p.border);
        int x = status_rc_.left + s(12);
        int right = status_rc_.right - s(12);
        const auto left_text = [&](const std::wstring& text, const COLORREF color, HFONT font) {
            const SIZE ts = measure_text(dc, text, font);
            draw_text(dc, text, {x, status_rc_.top, x + ts.cx + 1, status_rc_.bottom}, color, font, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            x += ts.cx + s(16);
        };
        const auto right_text = [&](const std::wstring& text, const COLORREF color, HFONT font) {
            const SIZE ts = measure_text(dc, text, font);
            draw_text(dc, text, {right - ts.cx - 1, status_rc_.top, right, status_rc_.bottom}, color, font, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            right -= ts.cx + s(16);
        };
        const std::wstring threads = trn(L"{0} thread", L"{0} threads", st.threads ? st.threads : settings_.threads);
        if (st.running) {
            const wchar_t* phase = st.stopping ? tr(L"Stopping…") : st.phase == chisel_app::Phase::Analyzing ? tr(L"Analyzing") : tr(L"Recompressing");
            left_text(phase, p.text, f.ui_small_semibold);
            left_text(trn(L"{1} of {0} file", L"{1} of {0} files", st.run_total, {std::to_wstring(st.run_done)}), p.text2, f.ui_small);
            const RECT track{x, (status_rc_.top + status_rc_.bottom) / 2 - s(2), x + s(200), (status_rc_.top + status_rc_.bottom) / 2 + s(2)};
            fill_round_rect(dc, track, s(2), p.bar_track);
            if (st.run_total > 0 && st.run_done > 0) {
                RECT fill = track;
                fill.right = track.left + static_cast<LONG>((track.right - track.left) * static_cast<double>(st.run_done) / static_cast<double>(st.run_total));
                fill_round_rect(dc, fill, s(2), p.accent);
            }
            right_text(st.dry_run ? trf(L"{0} · dry run", {threads}) : threads, p.text2, f.ui_small);
            right_text(trf(L"Saved so far: {0}", {format_bytes(st.run_bytes_saved)}), p.text2, f.ui_small);
            return;
        }
        if (adding_) {
            left_text(tr(L"Adding files…"), p.text, f.ui_small_semibold);
            return;
        }
        if (st.run_total > 0) {
            const bool stopped = st.run_stopped > 0;
            draw_glyph(dc, stopped ? glyph::stop : glyph::check, {x, status_rc_.top, x + s(14), status_rc_.bottom},
                       stopped ? p.text2 : p.ok, f.icons_small);
            x += s(14) + s(8);
            const std::wstring elapsed = format_duration(st.elapsed);
            left_text(stopped ? trf(L"Stopped after {0}", {elapsed})
                              : (st.dry_run ? trf(L"Dry run finished in {0}", {elapsed}) : trf(L"Finished in {0}", {elapsed})),
                      p.text, f.ui_small_semibold);
            std::wstring parts;
            const auto part = [&](const std::size_t n, const std::wstring& text) {
                if (n > 0) parts += (parts.empty() ? L"" : L", ") + text;
            };
            const std::size_t smaller = st.run_completed + st.run_simulated;
            part(smaller, st.dry_run ? trn(L"{0} would be smaller", L"{0} would be smaller", smaller)
                                     : trn(L"{0} smaller", L"{0} smaller", smaller));
            part(st.run_no_gain, trn(L"{0} no gain", L"{0} no gain", st.run_no_gain));
            part(st.run_skipped, trn(L"{0} skipped", L"{0} skipped", st.run_skipped));
            part(st.run_errors, trn(L"{0} error", L"{0} errors", st.run_errors));
            part(st.run_stopped, trn(L"{0} stopped", L"{0} stopped", st.run_stopped));
            left_text(trn(L"{0} file: {1}", L"{0} files: {1}", st.run_total, {parts}), p.text2, f.ui_small);
            right_text(threads, p.text2, f.ui_small);
            const double pct = st.run_bytes_total ? 100.0 * static_cast<double>(st.run_bytes_saved) / static_cast<double>(st.run_bytes_total) : 0;
            right_text(trf(st.dry_run ? L"Would save {0} ({1})" : L"Saved {0} ({1})", {format_bytes(st.run_bytes_saved), format_percent(pct)}),
                       p.ok, f.ui_small_semibold);
            return;
        }
        left_text(tr(L"Ready"), p.text2, f.ui_small);
        right_text(mode_label(settings_) + L" · " + threads + L" · chisel " + widen(CHISEL_VERSION), p.text2, f.ui_small);
    }

    void MainWindow::paint_empty(HDC dc) {
        const auto& p = palette();
        const auto& f = fonts(dpi_);
        fill_rect(dc, content_rc_, p.surface);
        const int cx = (content_rc_.left + content_rc_.right) / 2;
        const int cy = (content_rc_.top + content_rc_.bottom) / 2;
        const RECT box{cx - s(270), cy - s(150), cx + s(270), cy + s(140)};
        LOGBRUSH lb{BS_SOLID, p.dash, 0};
        HPEN pen = ExtCreatePen(PS_GEOMETRIC | PS_DASH | PS_ENDCAP_FLAT, static_cast<DWORD>(s(2)), &lb, 0, nullptr);
        auto old_pen = SelectObject(dc, pen);
        auto old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        RoundRect(dc, box.left, box.top, box.right, box.bottom, s(16), s(16));
        SelectObject(dc, old_pen);
        SelectObject(dc, old_brush);
        DeleteObject(pen);
        draw_glyph(dc, glyph::upload, {cx - s(30), box.top + s(30), cx + s(30), box.top + s(90)}, p.accent, f.icons_big);
        draw_text(dc, tr(L"Drop files or folders here"), {box.left, box.top + s(100), box.right, box.top + s(130)}, p.text, f.title,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        draw_text(dc, tr(L"Chisel makes them smaller without changing what's inside: images, audio, documents, archives, and the files they contain."),
                  {cx - s(200), box.top + s(136), cx + s(200), box.top + s(180)}, p.text2, f.ui, DT_CENTER | DT_WORDBREAK);
    }

    RECT MainWindow::chevron_rect(const int index) const {
        RECT rc{};
        if (index < 0 || static_cast<std::size_t>(index) >= visible_.size()) return rc;
        ListView_GetItemRect(list_, index, &rc, LVIR_BOUNDS);
        const Row& row = snap_.rows[visible_[static_cast<std::size_t>(index)]];
        const int left = rc.left + s(14) + static_cast<int>(row.depth) * s(20);
        const int top = rc.top + (rc.bottom - rc.top - s(24)) / 2;
        return {left, top, left + s(24), top + s(24)};
    }

    LRESULT MainWindow::draw_list_row(NMLVCUSTOMDRAW* cd) {
        switch (cd->nmcd.dwDrawStage) {
            case CDDS_PREPAINT: return CDRF_NOTIFYITEMDRAW;
            case CDDS_ITEMPREPAINT: break;
            default: return CDRF_DODEFAULT;
        }
        const int index = static_cast<int>(cd->nmcd.dwItemSpec);
        if (index < 0 || static_cast<std::size_t>(index) >= visible_.size()) return CDRF_SKIPDEFAULT;
        const auto& p = palette();
        const auto& f = fonts(dpi_);
        HDC dc = cd->nmcd.hdc;
        const Row& row = snap_.rows[visible_[static_cast<std::size_t>(index)]];
        RECT rc{};
        ListView_GetItemRect(list_, index, &rc, LVIR_BOUNDS);
        const bool selected = (ListView_GetItemState(list_, index, LVIS_SELECTED) & LVIS_SELECTED) != 0;
        COLORREF bg = p.surface;
        if (selected) bg = p.sel_bg;
        else if (row.status == Status::Error) bg = p.err_row_bg;
        else if (row.depth > 0) bg = p.child_bg;
        fill_rect(dc, rc, bg);
        fill_rect(dc, {rc.left, rc.bottom - 1, rc.right, rc.bottom}, p.row_line);

        int col_x[6];
        col_x[0] = rc.left;
        for (int i = 0; i < 5; ++i) col_x[i + 1] = col_x[i] + ListView_GetColumnWidth(list_, i);
        const int cy = (rc.top + rc.bottom) / 2;

        // name: chevron for containers, a dot for what was found inside, the format badge, the name
        const int level = static_cast<int>(row.depth);
        if (row.child_count > 0) {
            const RECT chev = chevron_rect(index);
            draw_glyph(dc, collapsed_.contains(row.id) ? glyph::chevron_right : glyph::chevron_down, chev, p.text2, f.icons_small);
        }
        int x = rc.left + s(12) + s(28) + level * s(20);
        if (row.depth > 0) {
            draw_dot(dc, x + s(2), cy, s(5), p.text3);
            x += s(5) + s(8);
        }
        const auto cat = category_for(row.extension);
        const RECT badge{x, cy - s(10), x + s(40), cy + s(10)};
        fill_round_rect(dc, badge, s(4), p.badge_bg[cat]);
        draw_text(dc, badge_text(row.extension), badge, p.badge_fg[cat], f.badge, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        x += s(40) + s(8);
        int name_right = col_x[1] - s(8);
        if (row.child_count > 0) {
            const std::wstring note = trn(L"{0} file inside", L"{0} files inside", row.child_count);
            const SIZE ns = measure_text(dc, note, f.ui_small);
            const std::wstring name = widen(row.name);
            const SIZE ts = measure_text(dc, name, f.ui);
            const int note_x = std::min(x + static_cast<int>(ts.cx) + s(8), name_right - static_cast<int>(ns.cx));
            draw_text(dc, note, {note_x, rc.top, name_right, rc.bottom}, p.text2, f.ui_small, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            name_right = note_x - s(8);
        }
        draw_text(dc, widen(row.name), {x, rc.top, name_right, rc.bottom}, row.status == Status::NoGain ? p.text2 : p.text, f.ui,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        draw_text(dc, format_bytes(row.size), {col_x[1], rc.top, col_x[2] - s(4), rc.bottom}, p.text2, f.ui,
                  DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        std::wstring after = L"—";
        if (row.size_after && (row.status == Status::Completed || row.status == Status::Simulated || row.status == Status::NoGain)) {
            after = format_bytes(*row.size_after);
        }
        draw_text(dc, after, {col_x[2], rc.top, col_x[3] - s(4), rc.bottom}, row.status == Status::Completed ? p.text : p.text3, f.ui,
                  DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

        x = col_x[3] + s(20);
        if (has_gain(row)) {
            const double pct = gain_percent(row);
            const RECT track{x, cy - s(2), x + s(56), cy + s(2)};
            fill_round_rect(dc, track, s(2), p.bar_track);
            RECT fill = track;
            fill.right = track.left + std::max<LONG>(s(2), static_cast<LONG>((track.right - track.left) * std::min(100.0, pct) / 100.0));
            fill_round_rect(dc, fill, s(2), p.bar_fill);
            draw_text(dc, format_percent(pct), {x + s(64), rc.top, col_x[4], rc.bottom}, p.ok, f.ui_small, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        } else {
            draw_text(dc, row.status == Status::NoGain ? L"0%" : L"—", {x, rc.top, col_x[4], rc.bottom}, p.text3, f.ui_small,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }

        const auto look = status_look(row);
        x = col_x[4];
        draw_dot(dc, x + s(4), cy, s(8), look.dot);
        draw_text(dc, look.label, {x + s(16), rc.top, col_x[5] - s(4), rc.bottom}, look.color, f.ui,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        return CDRF_SKIPDEFAULT;
    }

    LRESULT MainWindow::draw_header_item(NMCUSTOMDRAW* cd) {
        switch (cd->dwDrawStage) {
            case CDDS_PREPAINT: return CDRF_NOTIFYITEMDRAW;
            case CDDS_ITEMPREPAINT: break;
            default: return CDRF_DODEFAULT;
        }
        const auto& p = palette();
        const auto& f = fonts(dpi_);
        const int index = static_cast<int>(cd->dwItemSpec);
        RECT rc = cd->rc;
        fill_rect(cd->hdc, rc, p.surface);
        fill_rect(cd->hdc, {rc.left, rc.bottom - 1, rc.right, rc.bottom}, p.border);
        wchar_t text[64] = {};
        HDITEMW item{};
        item.mask = HDI_TEXT;
        item.pszText = text;
        item.cchTextMax = 64;
        Header_GetItem(cd->hdr.hwndFrom, index, &item);
        UINT align = DT_LEFT;
        if (index == 0) rc.left += s(12) + s(28);
        else if (index == 1 || index == 2) {
            align = DT_RIGHT;
            rc.right -= s(4);
        } else if (index == 3) rc.left += s(20);
        draw_text(cd->hdc, text, rc, p.text2, f.ui_small_semibold, align | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        return CDRF_SKIPDEFAULT;
    }

    LRESULT MainWindow::on_notify(NMHDR* hdr) {
        if (hdr->hwndFrom == list_) {
            switch (hdr->code) {
                case NM_CUSTOMDRAW:
                    return draw_list_row(reinterpret_cast<NMLVCUSTOMDRAW*>(hdr));
                case LVN_GETDISPINFOW: {
                    // text for screen readers and type-ahead search; drawing is custom
                    auto* di = reinterpret_cast<NMLVDISPINFOW*>(hdr);
                    const auto i = static_cast<std::size_t>(di->item.iItem);
                    if ((di->item.mask & LVIF_TEXT) && i < visible_.size()) {
                        static std::wstring buffer;
                        const Row& row = snap_.rows[visible_[i]];
                        switch (di->item.iSubItem) {
                            case 0: buffer = widen(row.name); break;
                            case 1: buffer = format_bytes(row.size); break;
                            case 2: buffer = row.size_after ? format_bytes(*row.size_after) : L""; break;
                            case 3: buffer = has_gain(row) ? format_percent(gain_percent(row)) : L""; break;
                            default: buffer = status_look(row).label; break;
                        }
                        di->item.pszText = buffer.data();
                    }
                    return 0;
                }
                case LVN_ITEMCHANGED: {
                    auto* nm = reinterpret_cast<NMLISTVIEW*>(hdr);
                    if (selecting_) return 0;
                    if ((nm->uChanged & LVIF_STATE) && (nm->uNewState & LVIS_SELECTED) && nm->iItem >= 0 &&
                        static_cast<std::size_t>(nm->iItem) < visible_.size()) {
                        selected_id_ = snap_.rows[visible_[static_cast<std::size_t>(nm->iItem)]].id;
                        update_inspector();
                    }
                    return 0;
                }
                case NM_CLICK: {
                    auto* nm = reinterpret_cast<NMITEMACTIVATE*>(hdr);
                    if (nm->iItem >= 0 && static_cast<std::size_t>(nm->iItem) < visible_.size()) {
                        const Row& row = snap_.rows[visible_[static_cast<std::size_t>(nm->iItem)]];
                        const RECT chev = chevron_rect(nm->iItem);
                        if (row.child_count > 0 && PtInRect(&chev, nm->ptAction)) toggle(row.id);
                    }
                    return 0;
                }
                case NM_DBLCLK: {
                    auto* nm = reinterpret_cast<NMITEMACTIVATE*>(hdr);
                    if (nm->iItem >= 0 && static_cast<std::size_t>(nm->iItem) < visible_.size()) {
                        const Row& row = snap_.rows[visible_[static_cast<std::size_t>(nm->iItem)]];
                        if (row.child_count > 0) toggle(row.id);
                        else if (row.parent == 0) on_command(IDC_OPEN);
                    }
                    return 0;
                }
                case LVN_KEYDOWN: {
                    auto* kd = reinterpret_cast<NMLVKEYDOWN*>(hdr);
                    const Row* row = selected_row();
                    if (!row) return 0;
                    if (kd->wVKey == VK_LEFT) {
                        if (row->child_count > 0 && !collapsed_.contains(row->id)) toggle(row->id);
                        else if (row->parent != 0) select_row(row->parent, true);
                    } else if (kd->wVKey == VK_RIGHT) {
                        if (row->child_count > 0 && collapsed_.contains(row->id)) toggle(row->id);
                    } else if (kd->wVKey == VK_DELETE) {
                        on_command(CMD_REMOVE);
                    }
                    return 0;
                }
                default:
                    break;
            }
        } else if (hdr->hwndFrom == ListView_GetHeader(list_)) {
            if (hdr->code == NM_CUSTOMDRAW) return draw_header_item(reinterpret_cast<NMCUSTOMDRAW*>(hdr));
            // column widths follow the window, not the mouse
            if (hdr->code == HDN_BEGINTRACKW || hdr->code == HDN_DIVIDERDBLCLICKW) return TRUE;
        }
        return 0;
    }

    void MainWindow::show_menu(const ToolItem& item, const bool start_menu) {
        HMENU menu = CreatePopupMenu();
        if (start_menu) {
            AppendMenuW(menu, MF_STRING, CMD_START, tr(L"Start\tF5"));
            AppendMenuW(menu, MF_STRING, CMD_DRY_RUN, tr(L"Dry run only (nothing is written)"));
        } else {
            AppendMenuW(menu, MF_STRING, CMD_CLEAR_ALL, tr(L"Clear all"));
            bool finished = false;
            for (const auto& r : snap_.rows) finished = finished || (r.parent == 0 && r.status != Status::Pending);
            AppendMenuW(menu, MF_STRING | (finished ? 0 : MF_GRAYED), CMD_CLEAR_FINISHED, tr(L"Clear finished"));
            AppendMenuW(menu, MF_STRING | (selected_row() && selected_row()->parent == 0 ? 0 : MF_GRAYED), CMD_REMOVE,
                        tr(L"Remove selected\tDel"));
        }
        POINT pt{item.rc.left, item.rc.bottom};
        ClientToScreen(hwnd_, &pt);
        const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, hwnd_, nullptr);
        DestroyMenu(menu);
        if (cmd) on_command(cmd);
    }

    void MainWindow::on_command(const int id) {
        const bool running = snap_.stats.running;
        switch (id) {
            case CMD_ADD_FILES:
            case IDC_EMPTY_FILES:
                if (!running) add_paths(pick_files(hwnd_));
                break;
            case CMD_ADD_FOLDER:
            case IDC_EMPTY_FOLDER:
                if (!running) add_paths(pick_folders(hwnd_, true));
                break;
            case CMD_START:
                if (!running) start(false);
                break;
            case CMD_DRY_RUN:
                if (!running) start(true);
                break;
            case CMD_STOP:
                session_.stop();
                break;
            case CMD_CLEAR:
                break;
            case CMD_CLEAR_ALL:
                if (!running) {
                    session_.clear();
                    collapsed_.clear();
                    selected_id_ = 0;
                    clear_info();
                }
                break;
            case CMD_CLEAR_FINISHED:
                if (!running) {
                    for (const auto& r : snap_.rows) {
                        if (r.parent == 0 && r.status != Status::Pending) session_.remove(r.id);
                    }
                    clear_info();
                }
                break;
            case CMD_REMOVE:
                if (const Row* row = selected_row(); row && row->parent == 0 && !running) {
                    session_.remove(row->id);
                    selected_id_ = 0;
                }
                break;
            case CMD_STATS:
                open_stats();
                break;
            case CMD_SETTINGS:
                open_settings();
                break;
            case CMD_ABOUT:
                show_about_window(hwnd_, instance_, session_.supported_extensions());
                break;
            case IDC_OPEN:
                if (const Row* row = selected_row(); row && row->parent == 0) {
                    show_in_folder(row->status == Status::Completed && !row->result.empty() ? row->result : row->path);
                }
                break;
            case IDC_COPY:
                if (const Row* row = selected_row()) {
                    std::wstring text = widen(row->name) + L"\r\n";
                    for (const auto& line : session_.row_logs(row->id)) text += widen(line) + L"\r\n";
                    copy_to_clipboard(hwnd_, text);
                }
                break;
            case IDC_FORMATS:
                show_formats_window(hwnd_, instance_, session_.supported_extensions());
                break;
            case IDC_INFO:
                if (info_kind_ == InfoKind::Errors && info_target_) {
                    select_row(info_target_, true);
                    SetFocus(list_);
                } else if (info_kind_ == InfoKind::Notice) {
                    std::wstring text;
                    std::size_t n = 0;
                    for (const auto& path : snap_.unsupported) {
                        if (++n > 40) {
                            text += L"…";
                            break;
                        }
                        text += path.filename().wstring() + L"\n";
                    }
                    MessageBoxW(hwnd_, text.c_str(), tr(L"Files not added"), MB_OK | MB_ICONINFORMATION);
                }
                break;
            default:
                break;
        }
    }

    void MainWindow::add_paths(std::vector<fs::path> paths) {
        if (paths.empty()) return;
        if (snap_.stats.running) {
            MessageBeep(MB_ICONWARNING);
            show_info(InfoKind::Notice, tr(L"Not added."), tr(L"Wait for the current run to finish to add files."), false);
            return;
        }
        clear_info();
        const bool recursive = settings_.recursive_folder_import;
        std::lock_guard lock(add_mutex_);
        add_queue_.push_back(std::move(paths));
        if (adding_) return;
        adding_ = true;
        if (add_thread_.joinable()) add_thread_.join();
        add_thread_ = std::thread([this, recursive, hwnd = hwnd_] {
            while (true) {
                std::vector<fs::path> batch;
                {
                    std::lock_guard inner(add_mutex_);
                    if (add_queue_.empty()) {
                        adding_ = false;
                        break;
                    }
                    batch = std::move(add_queue_.front());
                    add_queue_.erase(add_queue_.begin());
                }
                auto result = std::make_unique<chisel_app::Session::AddResult>(session_.add_paths(batch, recursive));
                if (PostMessageW(hwnd, WM_APP_ADDED, 0, reinterpret_cast<LPARAM>(result.get()))) result.release();
            }
            PostMessageW(hwnd, WM_APP_ADDED, 0, 0);
        });
        update_toolbar_state();
        InvalidateRect(hwnd_, &status_rc_, FALSE);
    }

    void MainWindow::start(const bool dry_run) {
        if (settings_.output_mode == chisel_app::OutputMode::OutputFolder && settings_.output_folder.empty() && !dry_run) {
            const auto folders = pick_folders(hwnd_, false);
            if (folders.empty()) return;
            settings_.output_folder = folders.front();
            chisel_app::save_settings(settings_, store_);
            store_.flush();
        }
        clear_info();
        const auto error = session_.start(settings_, {.dry_run = dry_run});
        if (error != chisel_app::StartError::None) {
            const std::wstring text = trf(L"Can't start: {0}.", {start_error_text(error)});
            MessageBoxW(hwnd_, text.c_str(), L"Chisel", MB_OK | MB_ICONWARNING);
        }
    }

    void MainWindow::open_settings() {
        run_settings_window(hwnd_, instance_, settings_, snap_.stats.running, [this](const chisel_app::Settings& updated) {
            settings_ = updated;
            chisel_app::save_settings(settings_, store_);
            store_.flush();
            InvalidateRect(hwnd_, &status_rc_, FALSE);
        });
    }

    void MainWindow::open_stats() {
        if (!stats_window_) stats_window_ = std::make_unique<StatsWindow>(instance_, stats_);
        stats_window_->show(hwnd_);
    }

    void MainWindow::show_info(const InfoKind kind, std::wstring title, std::wstring text, const bool action) {
        info_kind_ = kind;
        info_title_ = std::move(title);
        info_text_ = std::move(text);
        info_action_ = action;
        layout();
    }

    void MainWindow::clear_info() {
        if (info_kind_ == InfoKind::None) return;
        info_kind_ = InfoKind::None;
        info_target_ = 0;
        layout();
    }

    const Row* MainWindow::row_by_id(const std::uint64_t id) const {
        if (id == 0) return nullptr;
        for (const auto& r : snap_.rows) {
            if (r.id == id) return &r;
        }
        return nullptr;
    }

    const Row* MainWindow::selected_row() const { return row_by_id(selected_id_); }

    void MainWindow::rebuild_visible() {
        visible_.clear();
        int skip_depth = -1;
        for (std::size_t i = 0; i < snap_.rows.size(); ++i) {
            const Row& r = snap_.rows[i];
            if (skip_depth >= 0) {
                if (static_cast<int>(r.depth) > skip_depth) continue;
                skip_depth = -1;
            }
            visible_.push_back(i);
            if (r.child_count > 0 && collapsed_.contains(r.id)) skip_depth = static_cast<int>(r.depth);
        }
        ListView_SetItemCountEx(list_, static_cast<int>(visible_.size()), LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
        InvalidateRect(list_, nullptr, FALSE);
    }

    void MainWindow::restore_selection() {
        selecting_ = true;
        ListView_SetItemState(list_, -1, 0, LVIS_SELECTED);
        if (selected_id_) {
            bool found = false;
            for (std::size_t i = 0; i < visible_.size(); ++i) {
                if (snap_.rows[visible_[i]].id == selected_id_) {
                    ListView_SetItemState(list_, static_cast<int>(i), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                    found = true;
                    break;
                }
            }
            if (!found && !row_by_id(selected_id_)) selected_id_ = 0;
        }
        selecting_ = false;
    }

    void MainWindow::select_row(const std::uint64_t id, const bool ensure_visible) {
        // a row hidden in a collapsed container shows once its containers are open
        for (const Row* r = row_by_id(id); r && r->parent; r = row_by_id(r->parent)) collapsed_.erase(r->parent);
        selected_id_ = id;
        rebuild_visible();
        restore_selection();
        if (ensure_visible) {
            for (std::size_t i = 0; i < visible_.size(); ++i) {
                if (snap_.rows[visible_[i]].id == id) ListView_EnsureVisible(list_, static_cast<int>(i), FALSE);
            }
        }
        update_inspector();
    }

    void MainWindow::toggle(const std::uint64_t id) {
        if (!collapsed_.erase(id)) collapsed_.insert(id);
        rebuild_visible();
        restore_selection();
    }

    void MainWindow::update_toolbar_state() {
        const auto& st = snap_.stats;
        const bool empty = snap_.rows.empty();
        for (auto& t : tools_) {
            switch (t.command) {
                case CMD_ADD_FILES:
                case CMD_ADD_FOLDER: t.enabled = !st.running; break;
                case CMD_START: t.enabled = !st.running && st.pending > 0 && !adding_; break;
                case CMD_STOP: t.enabled = st.running && !st.stopping; break;
                case CMD_CLEAR: t.enabled = !st.running && !empty; break;
                default: t.enabled = true; break;
            }
        }
        InvalidateRect(hwnd_, &toolbar_rc_, FALSE);
    }

    void MainWindow::update_inspector() {
        const Row* row = selected_row();
        EnableWindow(open_button_, row && row->parent == 0);
        EnableWindow(copy_button_, row != nullptr);
        std::wstring text;
        if (row) {
            for (const auto& line : session_.row_logs(row->id)) text += widen(line) + L"\r\n";
            if (text.empty()) text = tr(L"Waiting to be processed");
        }
        if (text != log_text_) {
            const bool grew = !log_text_.empty() && text.starts_with(log_text_);
            log_text_ = text;
            SetWindowTextW(log_, text.c_str());
            if (grew) {
                const int len = GetWindowTextLengthW(log_);
                SendMessageW(log_, EM_SETSEL, len, len);
                SendMessageW(log_, EM_SCROLLCARET, 0, 0);
            }
        }
        InvalidateRect(hwnd_, &inspector_rc_, FALSE);
    }

    void MainWindow::update_taskbar() {
        if (!taskbar_) return;
        const auto& st = snap_.stats;
        if (st.running) {
            taskbar_->SetProgressState(hwnd_, st.run_total ? TBPF_NORMAL : TBPF_INDETERMINATE);
            if (st.run_total) taskbar_->SetProgressValue(hwnd_, st.run_done, st.run_total);
        } else {
            taskbar_->SetProgressState(hwnd_, TBPF_NOPROGRESS);
        }
    }

    void MainWindow::on_changed() {
        const bool was_empty = snap_.rows.empty();
        snap_ = session_.snapshot();
        if (snap_.rows.empty() != was_empty) layout();
        rebuild_visible();
        restore_selection();
        if (!selected_id_ && !visible_.empty() && snap_.stats.running) {
            // nothing chosen yet: follow the first file being worked on
            for (const auto i : visible_) {
                if (snap_.rows[i].status == Status::Processing) {
                    selected_id_ = snap_.rows[i].id;
                    restore_selection();
                    break;
                }
            }
        }
        update_toolbar_state();
        update_inspector();
        update_taskbar();
        InvalidateRect(hwnd_, &status_rc_, FALSE);
        if (was_running_ && !snap_.stats.running) on_run_finished();
        was_running_ = snap_.stats.running;
    }

    void MainWindow::on_run_finished() {
        const auto& st = snap_.stats;
        write_log_file();
        if (stats_window_) stats_window_->refresh();
        if (st.run_errors > 0) {
            const Row* first = row_by_id(st.first_error);
            const std::wstring title = trn(L"Finished with {0} error.", L"Finished with {0} errors.", st.run_errors);
            std::wstring text = first ? trf(L"{0} could not be recompressed; the original is unchanged.", {widen(first->name)}) : L"";
            info_target_ = st.first_error;
            show_info(InfoKind::Errors, title, text, first != nullptr);
        }
        if (GetForegroundWindow() != hwnd_) {
            FLASHWINFO fw{sizeof(fw), hwnd_, FLASHW_TRAY | FLASHW_TIMERNOFG, 0, 0};
            FlashWindowEx(&fw);
        }
        if (close_after_stop_) DestroyWindow(hwnd_);
    }

    void MainWindow::write_log_file() {
        const auto lines = session_.logs_since(log_seq_);
        if (lines.empty()) return;
        log_seq_ = lines.back().seq;
        const auto file = app_data_folder() / L"chisel-gui.log";
        std::error_code ec;
        if (fs::file_size(file, ec) > 4 * 1024 * 1024 && !ec) fs::rename(file, app_data_folder() / L"chisel-gui.old.log", ec);
        std::ofstream out(file, std::ios::binary | std::ios::app);
        if (!out) return;
        const std::time_t now = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &now);
        char stamp[32];
        std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &tm);
        out << "=== " << stamp << " run " << session_.last_run_id() << " (chisel " << CHISEL_VERSION << ")\r\n";
        const char* levels[] = {"ALL", "DEBUG", "INFO", "WARN", "ERROR", "OFF"};
        for (const auto& line : lines) {
            out << '[' << levels[std::clamp(line.level, 0, 5)] << "][" << line.tag << "] " << line.text << "\r\n";
        }
    }

} // namespace chisel_win
