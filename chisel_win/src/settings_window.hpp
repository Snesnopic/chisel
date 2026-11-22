#ifndef CHISEL_WIN_SETTINGS_WINDOW_HPP
#define CHISEL_WIN_SETTINGS_WINDOW_HPP

#include "chisel_app/settings.hpp"

#include <windows.h>

#include <functional>

namespace chisel_win {

    // modal, three tabs; apply is called for OK and Apply with the settings to keep
    void run_settings_window(HWND owner, HINSTANCE instance, const chisel_app::Settings& current, bool locked,
                             const std::function<void(const chisel_app::Settings&)>& apply);

} // namespace chisel_win

#endif // CHISEL_WIN_SETTINGS_WINDOW_HPP
