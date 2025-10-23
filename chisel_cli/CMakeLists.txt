set(CHISEL_CLI_SOURCES
        src/main.cpp
        src/cli/cli_parser.cpp
        src/cli/cli_parser.hpp
        src/report/report_generator.cpp
        src/report/report_generator.hpp
        src/utils/file_scanner.cpp
        src/utils/file_scanner.hpp
        src/utils/console_log_sink.hpp
        src/utils/file_log_sink.hpp
)

add_executable(chisel ${CHISEL_CLI_SOURCES})

target_include_directories(chisel PRIVATE
        $<TARGET_PROPERTY:libchisel,INTERFACE_INCLUDE_DIRECTORIES>
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(chisel PRIVATE libchisel ${CHISEL_LIBS})

if(WIN32)
    target_compile_definitions(chisel PRIVATE PLATFORM_WINDOWS)
elseif(APPLE)
    target_compile_definitions(chisel PRIVATE PLATFORM_APPLE)
elseif(UNIX AND NOT APPLE)
    target_compile_definitions(chisel PRIVATE PLATFORM_LINUX)
endif()

target_compile_features(chisel PRIVATE cxx_std_23)

set_target_properties(chisel PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/$<CONFIG>
)