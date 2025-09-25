cmake_minimum_required(VERSION 3.5)
project(monolith)

if (CMAKE_VERSION VERSION_GREATER_EQUAL "3.30")
    set(CMAKE_POLICY_VERSION_MINIMUM "3.5")
endif()

cmake_policy(SET CMP0069 NEW)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)
endif()

set(CMAKE_CXX_STANDARD 23)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    if (MSVC)
        add_compile_options(/Od /Zi /W4)
    else()
        add_compile_options(-O0 -g3 -fno-omit-frame-pointer -Wall -Wextra -Wpedantic)
        add_compile_options(-fsanitize=address,undefined)
        add_link_options(-fsanitize=address,undefined)
    endif()
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/debug)
elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
    if (MSVC)
        add_compile_options(/O2 /GL /W4)
    else()
        add_compile_options(-O3 -march=native -Wall -Wextra -Wpedantic)
    endif()
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/release)
endif()

include(FetchContent)
include(ExternalProject)

# libmagic
if(WIN32)
    find_path(LIBMAGIC_INCLUDE_DIR magic.h)
    find_library(LIBMAGIC_LIB magic)
    if(!LIBMAGIC_INCLUDE_DIR OR !LIBMAGIC_LIB)
        message(FATAL_ERROR "libmagic not found. Install with vcpkg (vcpkg install libmagic) or MSYS2.")
    endif()
else()
    ExternalProject_Add(libmagic
            URL https://astron.com/pub/file/file-5.45.tar.gz
            URL_HASH SHA256=fc97f51029bb0e2c9f4e3bffefdaf678f0e039ee872b9de5c002a6d09c784d82
            CONFIGURE_COMMAND ./configure --disable-shared --enable-static --prefix=<INSTALL_DIR>
            BUILD_COMMAND make
            INSTALL_COMMAND make install
            BUILD_IN_SOURCE 1
            INSTALL_DIR ${CMAKE_BINARY_DIR}/libmagic-install
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    set(LIBMAGIC_INCLUDE_DIR ${CMAKE_BINARY_DIR}/libmagic-install/include)
    set(LIBMAGIC_LIB_DIR ${CMAKE_BINARY_DIR}/libmagic-install/lib)
    set(LIBMAGIC_LIB magic)
endif()

# zlib
FetchContent_Declare(
        zlib
        GIT_REPOSITORY https://github.com/madler/zlib.git
        GIT_TAG        v1.3.1
)
FetchContent_MakeAvailable(zlib)

if(TARGET zlibstatic)
    set(ZLIB_TARGET zlibstatic)
elseif(TARGET zlib)
    set(ZLIB_TARGET zlib)
endif()

# libpng
set(PNG_SHARED OFF CACHE BOOL "" FORCE)
set(PNG_STATIC ON CACHE BOOL "" FORCE)
set(PNG_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
        libpng
        GIT_REPOSITORY https://github.com/glennrp/libpng.git
        GIT_TAG        v1.6.43
)
FetchContent_MakeAvailable(libpng)

# libogg
FetchContent_Declare(
        libogg
        GIT_REPOSITORY https://github.com/xiph/ogg.git
        GIT_TAG        v1.3.5
)
FetchContent_MakeAvailable(libogg)

# libFLAC
set(BUILD_CXXLIBS OFF CACHE BOOL "" FORCE)
set(BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
        libflac
        GIT_REPOSITORY https://github.com/xiph/flac.git
        GIT_TAG        1.4.3
)
FetchContent_MakeAvailable(libflac)
if(TARGET man)
    set_target_properties(man PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()

# wavpack
FetchContent_Declare(
        wavpack
        GIT_REPOSITORY https://github.com/dbry/WavPack.git
        GIT_TAG master
)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE)
set(WAVPACK_BUILD_CLI OFF CACHE BOOL "Build command line programs" FORCE)
set(WAVPACK_BUILD_DOCS OFF CACHE BOOL "Build documentation" FORCE)
set(WAVPACK_BUILD_TESTS OFF CACHE BOOL "Build tests" FORCE)

FetchContent_MakeAvailable(wavpack)

# libarchive
set(ENABLE_TAR OFF CACHE BOOL "" FORCE)
set(ENABLE_CPIO OFF CACHE BOOL "" FORCE)
set(ENABLE_CAT OFF CACHE BOOL "" FORCE)
set(ENABLE_TEST OFF CACHE BOOL "" FORCE)
set(ENABLE_ZLIB ON CACHE BOOL "" FORCE)
FetchContent_Declare(
        libarchive
        GIT_REPOSITORY https://github.com/libarchive/libarchive.git
        GIT_TAG        v3.8.1
)
FetchContent_MakeAvailable(libarchive)

# mozjpeg
ExternalProject_Add(mozjpeg
        GIT_REPOSITORY https://github.com/mozilla/mozjpeg.git
        GIT_TAG        v4.1.5
        CMAKE_ARGS
        -DENABLE_SHARED=FALSE
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
        -DCMAKE_INSTALL_PREFIX=${CMAKE_BINARY_DIR}/mozjpeg-install
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5
        UPDATE_DISCONNECTED 1
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(MOZJPEG_INCLUDE_DIR ${CMAKE_BINARY_DIR}/mozjpeg-install/include)
set(MOZJPEG_LIB_DIR ${CMAKE_BINARY_DIR}/mozjpeg-install/lib)

# libEBML
FetchContent_Declare(
        libebml
        GIT_REPOSITORY https://github.com/Matroska-Org/libebml.git
        GIT_TAG        release-1.4.5
)
FetchContent_MakeAvailable(libebml)

# libMatroska
FetchContent_Declare(
        libmatroska
        GIT_REPOSITORY https://github.com/Matroska-Org/libmatroska.git
        GIT_TAG        release-1.7.1
)
FetchContent_GetProperties(libmatroska)
if(NOT libmatroska_POPULATED)
    FetchContent_Populate(libmatroska)
    file(READ  "${libmatroska_SOURCE_DIR}/CMakeLists.txt" _lmkv_cmake)
    string(REPLACE "find_package(EBML" "# find_package(EBML" _lmkv_cmake "${_lmkv_cmake}")
    string(REPLACE "EBML::ebml" "ebml" _lmkv_cmake "${_lmkv_cmake}")
    file(WRITE "${libmatroska_SOURCE_DIR}/CMakeLists.txt" "${_lmkv_cmake}")
    add_subdirectory(${libmatroska_SOURCE_DIR} ${libmatroska_BINARY_DIR})
endif()

include(FetchContent)

# libwebp
FetchContent_Declare(
        libwebp
        GIT_REPOSITORY https://chromium.googlesource.com/webm/libwebp
        GIT_TAG main
)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE)
set(WEBP_BUILD_ANIM_UTILS OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_CWEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_DWEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_GIF2WEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_IMG2WEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_VWEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_WEBPINFO OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_EXTRAS OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_TESTS OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(libwebp)

# monolith
add_executable(monolith
        src/main.cpp
        src/encoder/encoder.hpp
        src/encoder/flac_encoder.cpp
        src/encoder/flac_encoder.hpp
        src/encoder/png_encoder.cpp
        src/encoder/png_encoder.hpp
        src/utils/file_type.cpp
        src/utils/file_type.hpp
        src/utils/thread_pool.hpp
        src/utils/thread_pool.cpp
        src/encoder/jpeg_encoder.cpp
        src/encoder/jpeg_encoder.hpp
        src/utils/logger.hpp
        src/utils/logger.cpp
        src/containers/archive_handler.cpp
        src/containers/archive_handler.hpp
        src/utils/archive_formats.hpp
        src/cli/cli_parser.cpp
        src/cli/cli_parser.hpp
        src/utils/file_scanner.cpp
        src/utils/file_scanner.hpp
        src/report/report_generator.cpp
        src/report/report_generator.hpp
        src/containers/container.hpp
        src/containers/mkv_handler.cpp
        src/containers/mkv_handler.hpp
        src/encoder/wav_encoder.cpp
        src/encoder/wav_encoder.hpp
        src/encoder/webp_encoder.cpp
        src/encoder/webp_encoder.hpp
)

target_include_directories(matroska PRIVATE
        ${libmatroska_BINARY_DIR}
        ${libebml_BINARY_DIR}
)
target_include_directories(monolith PRIVATE
        ${libmatroska_BINARY_DIR}
        ${libebml_BINARY_DIR}
)

add_dependencies(monolith mozjpeg ${ZLIB_TARGET})
if(NOT WIN32)
    add_dependencies(monolith libmagic)
endif()

target_include_directories(matroska PRIVATE
        ${libebml_BINARY_DIR}
)
target_include_directories(monolith PRIVATE
        ${LIBMAGIC_INCLUDE_DIR}
        ${libflac_SOURCE_DIR}/include
        ${libflac_BINARY_DIR}/include
        ${libpng_SOURCE_DIR}
        ${libpng_BINARY_DIR}
        ${MOZJPEG_INCLUDE_DIR}
        ${libarchive_SOURCE_DIR}/libarchive
        ${libarchive_BINARY_DIR}/libarchive
        ${libogg_SOURCE_DIR}/include
        ${libogg_BINARY_DIR}/include
        ${libebml_SOURCE_DIR}
        ${libmatroska_SOURCE_DIR}/src
)

if(NOT WIN32)
    target_link_directories(monolith PRIVATE ${LIBMAGIC_LIB_DIR})
endif()
target_link_directories(monolith PRIVATE ${MOZJPEG_LIB_DIR})

target_link_libraries(monolith
        ${LIBMAGIC_LIB}
        FLAC
        png_static
        jpeg
        archive_static
        ogg
        ${ZLIB_TARGET}
        ebml
        matroska
        wavpack
        webp
        libwebpmux
)
