#include "main_window.hpp"
#include "platform.hpp"
#include "ui.hpp"

#include "chisel_app/session.hpp"
#include "chisel_app/stats.hpp"

#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>

#include <filesystem>
#include <vector>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    // the window stays responsive while the run's threads work: they only post messages to it
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) return 1;
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES | ICC_UPDOWN_CLASS};
    InitCommonControlsEx(&icc);
    chisel_win::init_dark_mode_support();

    int exit_code = 0;
    {
        chisel_win::RegistryKeyValueStore store;
        chisel_app::FileStatsStore stats(chisel_win::app_data_folder() / L"stats.tsv");
        chisel_app::Session session(&stats);
        chisel_win::MainWindow window(instance, session, store, stats);
        if (!window.create(show)) {
            CoUninitialize();
            return 1;
        }

        // files given on the command line, e.g. dropped on the exe or sent from Explorer
        int argc = 0;
        if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
            std::vector<std::filesystem::path> paths;
            for (int i = 1; i < argc; ++i) paths.emplace_back(argv[i]);
            LocalFree(argv);
            window.add_paths(std::move(paths));
        }

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (window.pre_translate(msg)) continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        exit_code = static_cast<int>(msg.wParam);
    }
    chisel_win::reset_fonts();
    CoUninitialize();
    return exit_code;
}
