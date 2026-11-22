add_library(chisel_app STATIC
        include/chisel_app/paths.hpp
        include/chisel_app/settings.hpp
        include/chisel_app/stats.hpp
        include/chisel_app/formats.hpp
        include/chisel_app/output.hpp
        include/chisel_app/session.hpp
        src/paths.cpp
        src/settings.cpp
        src/stats.cpp
        src/formats.cpp
        src/output.cpp
        src/session.cpp
)

target_include_directories(chisel_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(chisel_app PUBLIC libchisel)
target_compile_features(chisel_app PUBLIC cxx_std_20)
target_compile_definitions(chisel_app PRIVATE CHISEL_VERSION="${PROJECT_VERSION}")
if(WIN32)
    target_compile_definitions(chisel_app PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
endif()

if(CHISEL_BUILD_APP_HEADLESS)
    add_executable(chisel_app_headless tools/headless.cpp)
    target_link_libraries(chisel_app_headless PRIVATE chisel_app)
    set_target_properties(chisel_app_headless PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/$<CONFIG>
    )
endif()
