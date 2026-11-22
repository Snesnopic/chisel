#ifndef CHISEL_WIN_INFO_WINDOWS_HPP
#define CHISEL_WIN_INFO_WINDOWS_HPP

#include <windows.h>

#include <string>
#include <vector>

namespace chisel_win {

    // modal, as ChiselSwift's: version, links to the repository and to the icon's author, the supported formats
    void show_about_window(HWND owner, HINSTANCE instance, const std::vector<std::string>& extensions);

    // modal and scrollable: chisel's extensions grouped by ChiselSwift's categories
    void show_formats_window(HWND owner, HINSTANCE instance, const std::vector<std::string>& extensions);

} // namespace chisel_win

#endif // CHISEL_WIN_INFO_WINDOWS_HPP
