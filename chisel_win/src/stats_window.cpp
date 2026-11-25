#include "stats_window.hpp"
#include "i18n.hpp"
#include "platform.hpp"
#include "ui.hpp"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <fstream>
#include <map>

namespace chisel_win {

    namespace {
        enum : int { ID_EXPORT = 500, ID_RESET };

        std::tm local_tm(const std::time_t t) {
            std::tm tm{};
            localtime_s(&tm, &t);
            return tm;
        }

        std::string key_of(const std::tm& tm, const bool month) {
            char key[16];
            if (month) snprintf(key, sizeof key, "%04d-%02d", tm.tm_year + 1900, tm.tm_mon + 1);
            else snprintf(key, sizeof key, "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
            return key;
        }

        std::wstring date_text(const std::tm& tm, const wchar_t* format) {
            SYSTEMTIME st{};
            st.wYear = static_cast<WORD>(tm.tm_year + 1900);
            st.wMonth = static_cast<WORD>(tm.tm_mon + 1);
            st.wDay = static_cast<WORD>(tm.tm_mday);
            wchar_t text[64] = {};
            GetDateFormatEx(date_locale(), 0, &st, format, text, 64, nullptr);
            return text;
        }

        std::uint64_t nice_ceiling(const std::uint64_t value) {
            if (value == 0) return 1;
            const double exponent = std::floor(std::log10(static_cast<double>(value)));
            const double base = std::pow(10.0, exponent);
            const double fraction = static_cast<double>(value) / base;
            const double nice = fraction <= 1 ? 1 : fraction <= 2 ? 2 : fraction <= 5 ? 5 : 10;
            return static_cast<std::uint64_t>(nice * base);
        }

        std::string csv(const std::string& field) {
            if (field.find_first_of(",\"\r\n") == std::string::npos) return field;
            std::string out = "\"";
            for (const char c : field) {
                if (c == '"') out += '"';
                out += c;
            }
            return out + "\"";
        }
    } // namespace

    StatsWindow::StatsWindow(HINSTANCE instance, chisel_app::StatsStore& stats) : instance_(instance), stats_(stats) {}

    StatsWindow::~StatsWindow() {
        if (hwnd_) DestroyWindow(hwnd_);
    }

    int StatsWindow::s(const int px) const { return scale(px, dpi_); }

    void StatsWindow::show(HWND owner) {
        if (!hwnd_) {
            static bool registered = false;
            if (!registered) {
                WNDCLASSEXW wc{};
                wc.cbSize = sizeof(wc);
                wc.lpfnWndProc = proc;
                wc.hInstance = instance_;
                wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
                wc.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(1));
                wc.lpszClassName = L"ChiselStatistics";
                RegisterClassExW(&wc);
                registered = true;
            }
            dpi_ = GetDpiForWindow(owner);
            CreateWindowExW(0, L"ChiselStatistics", tr(L"Chisel statistics"), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                            CW_USEDEFAULT, scale(960, dpi_), scale(680, dpi_), owner, nullptr, instance_, this);
            if (!hwnd_) return;
            export_button_ = CreateWindowExW(0, WC_BUTTONW, tr(L"Export CSV…"), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                             0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_EXPORT)), instance_, nullptr);
            reset_button_ = CreateWindowExW(0, WC_BUTTONW, tr(L"Reset…"), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                            0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_RESET)), instance_, nullptr);
            theme_changed();
            refresh();
            layout();
        }
        ShowWindow(hwnd_, IsIconic(hwnd_) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(hwnd_);
    }

    void StatsWindow::theme_changed() {
        if (!hwnd_) return;
        apply_window_theme(hwnd_);
        RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_FRAME);
    }

    void StatsWindow::refresh() {
        records_ = stats_.load();
        summary_ = chisel_app::summarize(records_);
        average_percent_ = 0;
        std::time_t first = 0;
        std::size_t counted = 0;
        std::map<std::string, std::uint64_t> by_format;
        for (const auto& r : records_) {
            if (r.original_size > 0) {
                average_percent_ += 100.0 * static_cast<double>(r.saved()) / static_cast<double>(r.original_size);
                ++counted;
            }
            if (first == 0 || r.timestamp < first) first = static_cast<std::time_t>(r.timestamp);
            by_format[r.extension] += r.saved();
        }
        if (counted) average_percent_ /= static_cast<double>(counted);
        since_ = records_.empty() ? std::wstring(tr(L"Nothing recorded yet"))
                                  : trf(L"All time, since {0}", {date_text(local_tm(first), L"MMMM yyyy")});

        formats_.clear();
        for (const auto& [ext, saved] : by_format) formats_.push_back({ext, saved});
        std::ranges::sort(formats_, [](const Format& a, const Format& b) { return a.saved > b.saved; });
        if (formats_.size() > 8) formats_.resize(8);

        std::map<std::string, std::uint64_t> day_totals;
        std::map<std::string, std::uint64_t> month_totals;
        for (const auto& d : chisel_app::by_period(records_, chisel_app::Period::Day)) day_totals[d.key] = d.summary.saved_bytes;
        for (const auto& m : chisel_app::by_period(records_, chisel_app::Period::Month)) month_totals[m.key] = m.summary.saved_bytes;
        std::tm today = local_tm(std::time(nullptr));
        today.tm_hour = 12;
        days_.clear();
        for (int i = 29; i >= 0; --i) {
            std::tm tm = today;
            tm.tm_mday -= i;
            std::mktime(&tm);
            Bar bar;
            bar.saved = day_totals[key_of(tm, false)];
            if (i % 7 == 0) bar.label = date_text(tm, L"d MMM");
            days_.push_back(bar);
        }
        months_.clear();
        for (int i = 11; i >= 0; --i) {
            std::tm tm = today;
            tm.tm_mday = 1;
            tm.tm_mon -= i;
            std::mktime(&tm);
            months_.push_back({month_totals[key_of(tm, true)], date_text(tm, L"MMM")});
        }
        EnableWindow(export_button_, !records_.empty());
        EnableWindow(reset_button_, !records_.empty());
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void StatsWindow::layout() {
        RECT rc;
        GetClientRect(hwnd_, &rc);
        const int y = s(12);
        const int reset_w = std::max(s(100), button_width(reset_button_, ButtonStyle::Normal, true, dpi_));
        const int export_w = std::max(s(130), button_width(export_button_, ButtonStyle::Normal, true, dpi_));
        SetWindowPos(reset_button_, nullptr, rc.right - s(20) - reset_w, y, reset_w, s(32), SWP_NOZORDER);
        SetWindowPos(export_button_, nullptr, rc.right - s(20) - reset_w - s(12) - export_w, y, export_w, s(32), SWP_NOZORDER);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void StatsWindow::paint(HDC dc) {
        const auto& p = palette();
        const auto& f = fonts(dpi_);
        RECT rc;
        GetClientRect(hwnd_, &rc);
        fill_rect(dc, rc, p.window);
        const SIZE ts = measure_text(dc, tr(L"Statistics"), f.title);
        draw_text(dc, tr(L"Statistics"), {s(20), 0, s(20) + ts.cx + 1, s(56)}, p.text, f.title, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        draw_text(dc, since_, {s(20) + ts.cx + s(12), 0, rc.right - s(280), s(56)}, p.text2, f.ui,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        // four cards
        const int left = s(20);
        const int width = rc.right - s(40);
        const int card_w = (width - 3 * s(12)) / 4;
        const RECT cards{left, s(56), left + width, s(56) + s(92)};
        const auto card = [&](const int i, const std::wstring& label, const std::wstring& value, const std::wstring& note,
                              const COLORREF value_color, HFONT value_font) {
            const RECT r{cards.left + i * (card_w + s(12)), cards.top, cards.left + i * (card_w + s(12)) + card_w, cards.bottom};
            fill_round_rect(dc, r, s(8), p.card, p.border);
            draw_text(dc, label, {r.left + s(16), r.top + s(12), r.right - s(16), r.top + s(30)}, p.text2, f.ui_small, DT_LEFT | DT_SINGLELINE);
            draw_text(dc, value, {r.left + s(16), r.top + s(30), r.right - s(16), r.top + s(64)}, value_color, value_font,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            draw_text(dc, note, {r.left + s(16), r.top + s(64), r.right - s(16), r.bottom - s(8)}, p.text2, f.ui_small,
                      DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        };
        std::uint64_t original = 0;
        for (const auto& r : records_) original += r.original_size;
        card(0, tr(L"Space saved"), format_bytes(summary_.saved_bytes), trf(L"out of {0} processed", {format_bytes(original)}), p.ok,
             f.big_bold);
        card(1, tr(L"Files made smaller"), std::to_wstring(summary_.files), trn(L"in {0} run", L"in {0} runs", summary_.runs), p.text,
             f.big);
        card(2, tr(L"Average saving"), format_percent(average_percent_), tr(L"per file made smaller"), p.text, f.big);
        card(3, tr(L"Biggest save"), summary_.biggest ? format_bytes(summary_.biggest->saved()) : L"—",
             summary_.biggest ? trf(L"one {0} file", {badge_text(summary_.biggest->extension)}) : L"", p.text, f.big);

        // chart
        const int top = cards.bottom + s(12);
        const int bottom = rc.bottom - s(20);
        const RECT chart{left, top, left + width - s(12) - s(300), bottom};
        const RECT list{chart.right + s(12), top, left + width, bottom};
        fill_round_rect(dc, chart, s(8), p.card, p.border);
        fill_round_rect(dc, list, s(8), p.card, p.border);
        const auto& bars = by_month_ ? months_ : days_;
        std::uint64_t total = 0;
        std::uint64_t peak = 0;
        for (const auto& b : bars) {
            total += b.saved;
            peak = std::max(peak, b.saved);
        }
        draw_text(dc, by_month_ ? tr(L"Space saved per month") : tr(L"Space saved per day"),
                  {chart.left + s(16), chart.top + s(16), chart.right - s(200), chart.top + s(36)}, p.text, f.heading, DT_LEFT | DT_SINGLELINE);
        draw_text(dc, trf(by_month_ ? L"Last 12 months: {0}" : L"Last 30 days: {0}", {format_bytes(total)}),
                  {chart.left + s(16), chart.top + s(36), chart.right - s(200), chart.top + s(54)}, p.text2, f.ui_small, DT_LEFT | DT_SINGLELINE);
        const RECT seg{chart.right - s(16) - s(190), chart.top + s(18), chart.right - s(16), chart.top + s(50)};
        fill_round_rect(dc, seg, s(6), p.ctrl_bg, p.ctrl_border);
        seg_day_ = {seg.left + s(2), seg.top + s(2), (seg.left + seg.right) / 2, seg.bottom - s(2)};
        seg_month_ = {(seg.left + seg.right) / 2, seg.top + s(2), seg.right - s(2), seg.bottom - s(2)};
        if (!by_month_) fill_round_rect(dc, seg_day_, s(4), p.seg_on);
        else fill_round_rect(dc, seg_month_, s(4), p.seg_on);
        draw_text(dc, tr(L"By day"), seg_day_, by_month_ ? p.text2 : p.text, by_month_ ? f.ui : f.ui_semibold, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        draw_text(dc, tr(L"By month"), seg_month_, by_month_ ? p.text : p.text2, by_month_ ? f.ui_semibold : f.ui, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        const RECT plot{chart.left + s(16) + s(60), chart.top + s(78), chart.right - s(16), chart.bottom - s(16) - s(22)};
        if (plot.bottom - plot.top > s(40) && !bars.empty()) {
            const std::uint64_t ymax = nice_ceiling(peak);
            fill_rect(dc, {plot.left, plot.top, plot.right, plot.top + 1}, p.grid);
            fill_rect(dc, {plot.left, (plot.top + plot.bottom) / 2, plot.right, (plot.top + plot.bottom) / 2 + 1}, p.grid);
            fill_rect(dc, {plot.left, plot.bottom - 1, plot.right, plot.bottom}, p.axis);
            const auto ylabel = [&](const int y, const std::wstring& text) {
                draw_text(dc, text, {plot.left - s(60), y - s(8), plot.left - s(8), y + s(8)}, p.text2, f.ui_small,
                          DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            };
            ylabel(plot.top, format_bytes(ymax));
            ylabel((plot.top + plot.bottom) / 2, format_bytes(ymax / 2));
            ylabel(plot.bottom, L"0");
            const int n = static_cast<int>(bars.size());
            const int gap = by_month_ ? s(8) : s(3);
            const double slot = static_cast<double>(plot.right - plot.left - s(4)) / n;
            for (int i = 0; i < n; ++i) {
                const int x0 = plot.left + s(2) + static_cast<int>(i * slot);
                const int x1 = plot.left + s(2) + static_cast<int>((i + 1) * slot) - gap;
                const auto& b = bars[static_cast<std::size_t>(i)];
                if (b.saved > 0) {
                    const int h = std::max(s(2), static_cast<int>((plot.bottom - plot.top) * static_cast<double>(b.saved) / static_cast<double>(ymax)));
                    fill_round_rect(dc, {x0, plot.bottom - h, std::max(x0 + 1, x1), plot.bottom}, s(2), p.bar_fill);
                }
                if (!b.label.empty()) {
                    draw_text(dc, b.label, {x0, plot.bottom + s(5), x0 + s(80), plot.bottom + s(22)}, p.text2, f.ui_small, DT_LEFT | DT_SINGLELINE);
                }
            }
            if (peak == 0) {
                draw_text(dc, tr(L"No files made smaller in this period"), plot, p.text2, f.ui, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
        }

        // by format
        draw_text(dc, tr(L"Saved by format"), {list.left + s(16), list.top + s(16), list.right - s(16), list.top + s(36)}, p.text, f.heading,
                  DT_LEFT | DT_SINGLELINE);
        draw_text(dc, tr(L"All time"), {list.left + s(16), list.top + s(36), list.right - s(16), list.top + s(54)}, p.text2, f.ui_small,
                  DT_LEFT | DT_SINGLELINE);
        int y = list.top + s(70);
        const std::uint64_t most = formats_.empty() ? 1 : std::max<std::uint64_t>(1, formats_.front().saved);
        for (const auto& fmt : formats_) {
            if (y + s(20) > list.bottom - s(12)) break;
            const auto cat = category_for(fmt.extension);
            const RECT badge{list.left + s(16), y, list.left + s(56), y + s(20)};
            fill_round_rect(dc, badge, s(4), p.badge_bg[cat]);
            draw_text(dc, badge_text(fmt.extension), badge, p.badge_fg[cat], f.badge, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            const RECT track{badge.right + s(10), y + s(7), list.right - s(16) - s(70), y + s(13)};
            fill_round_rect(dc, track, s(3), p.bar_track);
            RECT fill = track;
            fill.right = track.left + std::max<LONG>(s(3), static_cast<LONG>((track.right - track.left) * static_cast<double>(fmt.saved) / static_cast<double>(most)));
            fill_round_rect(dc, fill, s(3), p.bar_fill);
            draw_text(dc, format_bytes(fmt.saved), {list.right - s(16) - s(64), y, list.right - s(16), y + s(20)}, p.text, f.ui_small,
                      DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            y += s(32);
        }
        if (formats_.empty()) {
            draw_text(dc, tr(L"Nothing yet"), {list.left + s(16), y, list.right - s(16), y + s(20)}, p.text2, f.ui, DT_LEFT | DT_SINGLELINE);
        }
    }

    void StatsWindow::export_csv() {
        const auto file = pick_save_file(hwnd_, tr(L"chisel-statistics.csv"), L"CSV", L"*.csv");
        if (!file) return;
        std::ofstream out(*file, std::ios::binary | std::ios::trunc);
        if (!out) {
            MessageBoxW(hwnd_, tr(L"Can't write that file."), tr(L"Chisel statistics"), MB_OK | MB_ICONWARNING);
            return;
        }
        out << "\xEF\xBB\xBF" << "date,extension,original_size,compressed_size,saved,saved_percent,duration_ms,run_id,file_name\r\n";
        for (const auto& r : records_) {
            const std::tm tm = local_tm(static_cast<std::time_t>(r.timestamp));
            char date[32];
            std::strftime(date, sizeof date, "%Y-%m-%d %H:%M:%S", &tm);
            char pct[32];
            snprintf(pct, sizeof pct, "%.2f", r.original_size ? 100.0 * static_cast<double>(r.saved()) / static_cast<double>(r.original_size) : 0.0);
            out << date << ',' << csv(r.extension) << ',' << r.original_size << ',' << r.compressed_size << ',' << r.saved() << ','
                << pct << ',' << r.duration_ms << ',' << csv(r.run_id) << ',' << csv(r.file_name) << "\r\n";
        }
    }

    void StatsWindow::reset() {
        if (MessageBoxW(hwnd_, tr(L"Delete every statistic Chisel has recorded? This can't be undone."), tr(L"Chisel statistics"),
                        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
            return;
        }
        stats_.clear();
        refresh();
    }

    LRESULT CALLBACK StatsWindow::proc(HWND hwnd, const UINT msg, const WPARAM wp, const LPARAM lp) {
        StatsWindow* self = nullptr;
        if (msg == WM_NCCREATE) {
            self = static_cast<StatsWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<StatsWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        if (msg == WM_NCDESTROY) {
            if (self) {
                self->hwnd_ = nullptr;
                self->export_button_ = nullptr;
                self->reset_button_ = nullptr;
            }
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
        return self ? self->handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
    }

    LRESULT StatsWindow::handle(const UINT msg, const WPARAM wp, const LPARAM lp) {
        switch (msg) {
            case WM_SIZE:
                layout();
                return 0;
            case WM_GETMINMAXINFO: {
                auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
                mm->ptMinTrackSize = {s(760), s(520)};
                return 0;
            }
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
                BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
                SelectObject(mem, old);
                DeleteObject(bmp);
                DeleteDC(mem);
                EndPaint(hwnd_, &ps);
                return 0;
            }
            case WM_LBUTTONDOWN: {
                const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                if (PtInRect(&seg_day_, pt)) by_month_ = false;
                else if (PtInRect(&seg_month_, pt)) by_month_ = true;
                else return 0;
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            case WM_DRAWITEM:
                draw_button(*reinterpret_cast<DRAWITEMSTRUCT*>(lp), ButtonStyle::Normal,
                            reinterpret_cast<DRAWITEMSTRUCT*>(lp)->CtlID == ID_EXPORT ? glyph::download : glyph::trash, dpi_);
                return TRUE;
            case WM_CTLCOLORBTN: {
                HDC dc = reinterpret_cast<HDC>(wp);
                SetBkColor(dc, palette().window);
                SetDCBrushColor(dc, palette().window);
                return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
            }
            case WM_COMMAND:
                if (LOWORD(wp) == ID_EXPORT) export_csv();
                else if (LOWORD(wp) == ID_RESET) reset();
                return 0;
            case WM_DPICHANGED: {
                dpi_ = HIWORD(wp);
                const auto* r = reinterpret_cast<RECT*>(lp);
                SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
                layout();
                return 0;
            }
            default:
                break;
        }
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }

} // namespace chisel_win
