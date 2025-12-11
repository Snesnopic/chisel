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
        src/utils/color.hpp
        src/cli/CLI11.hpp
)
if(WIN32)
    list(APPEND CHISEL_CLI_SOURCES chisel.rc)
endif()
add_executable(chisel ${CHISEL_CLI_SOURCES})

if(APPLE)
    add_custom_command(TARGET chisel POST_BUILD
            COMMAND strip -x $<TARGET_FILE:chisel>
            COMMENT "Stripping symbols from chisel (macOS)")
endif()

target_link_libraries(chisel PRIVATE libchisel)

target_include_directories(chisel PRIVATE
        $<TARGET_PROPERTY:libchisel,INTERFACE_INCLUDE_DIRECTORIES>
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

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

if(NOT WIN32)
    find_program(HELP2MAN_EXECUTABLE help2man)

    if(HELP2MAN_EXECUTABLE)
        message(STATUS "Found help2man, manpage will be generated: ${HELP2MAN_EXECUTABLE}")

        set(MANPAGE_FILE "${CMAKE_CURRENT_BINARY_DIR}/chisel.1")

        add_custom_command(
                OUTPUT ${MANPAGE_FILE}
                COMMAND ${HELP2MAN_EXECUTABLE}
                --no-info
                --name="Lossless file recompressor"
                $<TARGET_FILE:chisel> > ${MANPAGE_FILE}
                DEPENDS chisel
                COMMENT "Generating manpage chisel.1 using help2man"
                VERBATIM
        )
        add_custom_target(manpage ALL DEPENDS ${MANPAGE_FILE})

        install(
                FILES ${MANPAGE_FILE}
                DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/man/man1
                COMPONENT Runtime
        )

    else()
        message(WARNING "help2man not found. Manpage (chisel.1) will not be generated.")
    endif()

endif()