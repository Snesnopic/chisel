#include "i18n.hpp"
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
#include <string_view>
#include <vector>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    // the window stays responsive while the run's threads work: they only post messages to it
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) return 1;

    // --lang=it or CHISEL_LANG=it picks the language, otherwise Windows' display language does
    std::vector<std::filesystem::path> paths;
    std::wstring language;
    wchar_t env[32] = {};
    if (const DWORD n = GetEnvironmentVariableW(L"CHISEL_LANG", env, 32); n > 0 && n < 32) language = env;
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 1; i < argc; ++i) {
            const std::wstring_view arg = argv[i];
            if (arg.starts_with(L"--lang=")) language = arg.substr(7);
            else paths.emplace_back(arg);
        }
        LocalFree(argv);
    }
    chisel_win::init_language(language);

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
        if (!paths.empty()) window.add_paths(std::move(paths));

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
