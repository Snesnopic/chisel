configure_file(res/version.h.in ${CMAKE_CURRENT_BINARY_DIR}/version.h @ONLY)

add_executable(chisel_win WIN32
        src/main.cpp
        src/i18n.cpp
        src/i18n.hpp
        src/ui.cpp
        src/ui.hpp
        src/platform.cpp
        src/platform.hpp
        src/main_window.cpp
        src/main_window.hpp
        src/settings_window.cpp
        src/settings_window.hpp
        src/stats_window.cpp
        src/stats_window.hpp
        src/info_windows.cpp
        src/info_windows.hpp
        res/chisel_win.rc
)

target_include_directories(chisel_win PRIVATE src res ${CMAKE_CURRENT_BINARY_DIR})
target_compile_definitions(chisel_win PRIVATE
        UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00
        CHISEL_VERSION="${PROJECT_VERSION}"
)
target_compile_features(chisel_win PRIVATE cxx_std_20)
target_link_libraries(chisel_win PRIVATE chisel_app
        user32 gdi32 comctl32 uxtheme dwmapi shlwapi shell32 ole32 uuid advapi32
)
# the manifest comes from the resource file
if(MSVC)
    target_link_options(chisel_win PRIVATE /MANIFEST:NO)
endif()

set_target_properties(chisel_win PROPERTIES
        OUTPUT_NAME "Chisel"
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/$<CONFIG>
)
