cmake_minimum_required(VERSION 3.20)

project(monolith VERSION 0.1 LANGUAGES C CXX)

if (CMAKE_VERSION VERSION_GREATER_EQUAL "3.30")
    set(CMAKE_POLICY_VERSION_MINIMUM "3.5")
endif()

cmake_policy(SET CMP0069 NEW)
if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
    set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# ---------------- LTO detection and common flags (non-invasive) -------------
include(CheckCCompilerFlag)
include(CheckCXXCompilerFlag)

option(MONOLITH_USE_THINLTO "Prefer -flto=thin when available" ON)

set(MONOLITH_LTO_FLAG "")
if(MONOLITH_USE_THINLTO)
    check_c_compiler_flag("-flto=thin" MONOLITH_C_FLTO_THIN)
    check_cxx_compiler_flag("-flto=thin" MONOLITH_CXX_FLTO_THIN)
    if(MONOLITH_C_FLTO_THIN AND MONOLITH_CXX_FLTO_THIN)
        set(MONOLITH_LTO_FLAG "-flto=thin")
    endif()
endif()

if(MONOLITH_LTO_FLAG STREQUAL "")
    check_c_compiler_flag("-flto" MONOLITH_C_FLTO)
    check_cxx_compiler_flag("-flto" MONOLITH_CXX_FLTO)
    if(MONOLITH_C_FLTO AND MONOLITH_CXX_FLTO)
        set(MONOLITH_LTO_FLAG "-flto")
    endif()
endif()

# check for linker plugin flag (helps with LTO)
check_c_compiler_flag("-fuse-linker-plugin" MONOLITH_C_FUSE_PLUGIN)
check_cxx_compiler_flag("-fuse-linker-plugin" MONOLITH_CXX_FUSE_PLUGIN)
if(MONOLITH_C_FUSE_PLUGIN AND MONOLITH_CXX_FUSE_PLUGIN)
    set(MONOLITH_FUSE_LINKER_PLUGIN_FLAG "-fuse-linker-plugin")
else()
    set(MONOLITH_FUSE_LINKER_PLUGIN_FLAG "")
endif()

# --- DEFINE FLAGS AS LISTS (not a single quoted string) ---------------------
# use list form so add_compile_options expands to multiple args correctly
set(MONOLITH_COMMON_OPT_FLAGS -ffunction-sections -fdata-sections -fvisibility=hidden)

# linker flags: platform-appropriate list
if(APPLE)
    # macOS linker: dead_strip removes unused code
    set(MONOLITH_COMMON_LINK_FLAGS -Wl,-dead_strip)
elseif(WIN32)
    set(MONOLITH_COMMON_LINK_FLAGS) # empty on MSVC
else()
    set(MONOLITH_COMMON_LINK_FLAGS -Wl,--gc-sections -Wl,--as-needed)
endif()

# end lists -----------------------------------------------------------------
if(MONOLITH_LTO_FLAG)
    message(STATUS "monolith: using LTO flag = ${MONOLITH_LTO_FLAG}")
    # MONOLITH_LINK_FLAGS is a list too
    set(MONOLITH_LINK_FLAGS ${MONOLITH_LTO_FLAG} ${MONOLITH_FUSE_LINKER_PLUGIN_FLAG} ${MONOLITH_COMMON_LINK_FLAGS})
else()
    message(STATUS "monolith: LTO not available on this compiler")
    set(MONOLITH_LINK_FLAGS ${MONOLITH_COMMON_LINK_FLAGS})
endif()

# MONOLITH_PROPAGATE_* exclude -flto to avoid breaking subproject checks, keep lists
set(MONOLITH_PROPAGATE_C_FLAGS ${MONOLITH_COMMON_OPT_FLAGS})
set(MONOLITH_PROPAGATE_CXX_FLAGS ${MONOLITH_COMMON_OPT_FLAGS})
set(MONOLITH_PROPAGATE_LINK_FLAGS ${MONOLITH_COMMON_LINK_FLAGS})

# For cache variables we need a space-separated string: replace ';' with ' '
string(REPLACE ";" " " MONOLITH_PROPAGATE_C_FLAGS_STR "${MONOLITH_PROPAGATE_C_FLAGS}")
string(REPLACE ";" " " MONOLITH_PROPAGATE_CXX_FLAGS_STR "${MONOLITH_PROPAGATE_CXX_FLAGS}")
string(REPLACE ";" " " MONOLITH_PROPAGATE_LINK_FLAGS_STR "${MONOLITH_PROPAGATE_LINK_FLAGS}")

message(STATUS "monolith: propagate C flags (no-LTO) = ${MONOLITH_PROPAGATE_C_FLAGS_STR}")
message(STATUS "monolith: propagate link flags (no-LTO) = ${MONOLITH_PROPAGATE_LINK_FLAGS_STR}")

# Prepare environment variables for autotools builds (ExternalProject)
set(EXTERNAL_AUTOTOOLS_CFLAGS "${MONOLITH_PROPAGATE_C_FLAGS_STR}")
set(EXTERNAL_AUTOTOOLS_CXXFLAGS "${MONOLITH_PROPAGATE_CXX_FLAGS_STR}")
set(EXTERNAL_AUTOTOOLS_LDFLAGS "${MONOLITH_PROPAGATE_LINK_FLAGS_STR}")
# ---------------------------------------------------------------------------

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    if (MSVC)
        add_compile_options(/Od /Zi /W4)
    else()
        add_compile_options(-O0 -g3 -fno-omit-frame-pointer -Wall -Wextra -Wpedantic)
        add_compile_options(-fsanitize=address,undefined)
        add_link_options(-fsanitize=address,undefined)
    endif()
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/debug)

elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
    if (MSVC)
        add_compile_options(/O2 /GL /W4)
    else()
        # add list flags safely; MONOLITH_COMMON_OPT_FLAGS is a list and expands properly
        add_compile_options(-O3 -march=native -Wall -Wextra -Wpedantic ${MONOLITH_COMMON_OPT_FLAGS})
        if(MONOLITH_LTO_FLAG)
            add_compile_options(${MONOLITH_LTO_FLAG} ${MONOLITH_FUSE_LINKER_PLUGIN_FLAG})
        endif()
        # MONOLITH_LINK_FLAGS is a list
        add_link_options(${MONOLITH_LINK_FLAGS})
    endif()
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/release)

elseif(CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
    if (MSVC)
        add_compile_options(/O1 /GL /W4)
        add_link_options(/LTCG)
    else()
        add_compile_options(-Os -Wall -Wextra -Wpedantic ${MONOLITH_COMMON_OPT_FLAGS})
        if(MONOLITH_LTO_FLAG)
            add_compile_options(${MONOLITH_LTO_FLAG} ${MONOLITH_FUSE_LINKER_PLUGIN_FLAG})
        endif()
        add_link_options(${MONOLITH_LINK_FLAGS})
    endif()
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/size)
endif()

include(FetchContent)
include(ExternalProject)

# Export propagated flags into CMake cache so FetchContent subprojects inherit them.
# NOTE: we intentionally do NOT include -flto in these propagated strings to avoid
# confusing subprojects' try_compile/try_run checks.
set(_new_c_flags "${CMAKE_C_FLAGS} ${MONOLITH_PROPAGATE_C_FLAGS_STR}")
set(_new_cxx_flags "${CMAKE_CXX_FLAGS} ${MONOLITH_PROPAGATE_CXX_FLAGS_STR}")
set(_new_exe_linker_flags "${CMAKE_EXE_LINKER_FLAGS} ${MONOLITH_PROPAGATE_LINK_FLAGS_STR}")

if(NOT "${_new_c_flags}" STREQUAL "")
    set(CMAKE_C_FLAGS "${_new_c_flags}" CACHE STRING "Propagated C flags for external projects" FORCE)
endif()
if(NOT "${_new_cxx_flags}" STREQUAL "")
    set(CMAKE_CXX_FLAGS "${_new_cxx_flags}" CACHE STRING "Propagated CXX flags for external projects" FORCE)
endif()
if(NOT "${_new_exe_linker_flags}" STREQUAL "")
    set(CMAKE_EXE_LINKER_FLAGS "${_new_exe_linker_flags}" CACHE STRING "Propagated linker flags for external projects" FORCE)
endif()

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
            # pass CFLAGS/CXXFLAGS/LDFLAGS so autotools builds see our propagated flags
            CONFIGURE_ENV CFLAGS=${EXTERNAL_AUTOTOOLS_CFLAGS} CXXFLAGS=${EXTERNAL_AUTOTOOLS_CXXFLAGS} LDFLAGS=${EXTERNAL_AUTOTOOLS_LDFLAGS}
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
        GIT_TAG        v1.6.50
)
FetchContent_MakeAvailable(libpng)

# libogg
FetchContent_Declare(
        libogg
        GIT_REPOSITORY https://github.com/xiph/ogg.git
        GIT_TAG        v1.3.6
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
        GIT_TAG        1.5.0
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

set(FFMPEG_PREFIX ${CMAKE_BINARY_DIR}/ffmpeg-install)
ExternalProject_Add(ffmpeg
        GIT_REPOSITORY https://github.com/FFmpeg/FFmpeg.git
        GIT_TAG n7.1.2
        PREFIX ${CMAKE_BINARY_DIR}/ffmpeg
        CONFIGURE_COMMAND ./configure
        --prefix=${FFMPEG_PREFIX}
        --disable-shared
        --enable-static
        --disable-programs
        --disable-doc
        --disable-debug
        --disable-everything
        --enable-muxer=mp4
        --enable-demuxer=mov
        --enable-protocol=file
        BUILD_COMMAND make -j4
        INSTALL_COMMAND make install
        BUILD_IN_SOURCE 1
        UPDATE_DISCONNECTED 1
        BUILD_BYPRODUCTS
        ${FFMPEG_PREFIX}/lib/libavcodec.a
        ${FFMPEG_PREFIX}/lib/libavformat.a
        ${FFMPEG_PREFIX}/lib/libavutil.a
)

add_library(ffmpeg_avcodec STATIC IMPORTED GLOBAL)
add_library(ffmpeg_avformat STATIC IMPORTED GLOBAL)
add_library(ffmpeg_avutil STATIC IMPORTED GLOBAL)

add_dependencies(ffmpeg_avcodec ffmpeg)
add_dependencies(ffmpeg_avformat ffmpeg)
add_dependencies(ffmpeg_avutil ffmpeg)

file(MAKE_DIRECTORY ${FFMPEG_PREFIX}/include)

set_target_properties(ffmpeg_avcodec PROPERTIES
        IMPORTED_LOCATION ${FFMPEG_PREFIX}/lib/libavcodec.a
        INTERFACE_INCLUDE_DIRECTORIES ${FFMPEG_PREFIX}/include
)
set_target_properties(ffmpeg_avformat PROPERTIES
        IMPORTED_LOCATION ${FFMPEG_PREFIX}/lib/libavformat.a
        INTERFACE_INCLUDE_DIRECTORIES ${FFMPEG_PREFIX}/include
)
set_target_properties(ffmpeg_avutil PROPERTIES
        IMPORTED_LOCATION ${FFMPEG_PREFIX}/lib/libavutil.a
        INTERFACE_INCLUDE_DIRECTORIES ${FFMPEG_PREFIX}/include
)

# qpdf
set(QPDF_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(QPDF_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(QPDF_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(QPDF_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(QPDF_BUILD_SHARED OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
        qpdf
        GIT_REPOSITORY https://github.com/qpdf/qpdf.git
        GIT_TAG main
)
FetchContent_MakeAvailable(qpdf)

# zopfli
FetchContent_Declare(
        zopfli
        GIT_REPOSITORY https://github.com/google/zopfli.git
        GIT_TAG master
)
FetchContent_MakeAvailable(zopfli)

# JPEG XL
FetchContent_Declare(
        libjxl
        GIT_REPOSITORY https://github.com/libjxl/libjxl.git
        GIT_TAG v0.11.1
)

set(JPEGXL_ENABLE_TOOLS OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_DEVTOOLS OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_PLUGINS OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_MANPAGES OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_BENCHMARK OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_TESTS OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(libjxl)

# libtiff
FetchContent_Declare(
        libtiff
        GIT_REPOSITORY https://gitlab.com/libtiff/libtiff.git
        GIT_TAG v4.7.1
)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(tiff-tools OFF CACHE BOOL "" FORCE)
set(tiff-tests OFF CACHE BOOL "" FORCE)
set(tiff-docs OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(libtiff)


FetchContent_Declare(
        sqlite3
        GIT_REPOSITORY https://github.com/sjinks/sqlite3-cmake.git
        GIT_TAG        v3.50.2
)

FetchContent_MakeAvailable(sqlite3)


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
        src/encoder/wavpack_encoder.cpp
        src/encoder/wavpack_encoder.hpp
        src/encoder/webp_encoder.cpp
        src/encoder/webp_encoder.hpp
        src/encoder/pdf_encoder.cpp
        src/encoder/pdf_encoder.hpp
        src/encoder/zopflipng_encoder.cpp
        src/encoder/zopflipng_encoder.hpp
        src/encoder/jxl_encoder.cpp
        src/encoder/jxl_encoder.hpp
        src/encoder/tiff_encoder.cpp
        src/encoder/tiff_encoder.hpp
        src/encoder/sqlite_encoder.cpp
        src/encoder/sqlite_encoder.hpp
        src/utils/encoder_registry.cpp
        src/utils/encoder_registry.hpp
        src/containers/docx_handler.cpp
        src/containers/docx_handler.hpp
        src/containers/xlsx_handler.cpp
        src/containers/xlsx_handler.hpp
        src/containers/pptx_handler.cpp
        src/containers/pptx_handler.hpp
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

target_include_directories(monolith PRIVATE
        ${qpdf_SOURCE_DIR}/include
        ${qpdf_BINARY_DIR}/include
)

target_include_directories(monolith PRIVATE
        ${zopfli_SOURCE_DIR}/include
        ${zopfli_BINARY_DIR}/include
)

target_include_directories(zopflipng PUBLIC
        ${zopfli_SOURCE_DIR}/src/zopflipng
        ${zopfli_SOURCE_DIR}/src/zopfli
)

# common libraries for all platforms
set(MONOLITH_LIBS
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
        ffmpeg_avcodec
        ffmpeg_avformat
        ffmpeg_avutil
        libqpdf
        libzopfli
        libzopflipng
        jxl
        jxl_threads
        tiff
        sqlite3
)

if(APPLE)
    # ffmpeg built with videotoolbox needs these frameworks
    find_library(COREFOUNDATION_FRAMEWORK CoreFoundation)
    find_library(COREVIDEO_FRAMEWORK CoreVideo)
    find_library(COREMEDIA_FRAMEWORK CoreMedia)
    find_library(VIDEOTOOLBOX_FRAMEWORK VideoToolbox)

    list(APPEND MONOLITH_LIBS
            ${COREFOUNDATION_FRAMEWORK}
            ${COREVIDEO_FRAMEWORK}
            ${COREMEDIA_FRAMEWORK}
            ${VIDEOTOOLBOX_FRAMEWORK}
    )
elseif(UNIX)
    # linux: ffmpeg often needs pthread, dl, m
    list(APPEND MONOLITH_LIBS
            pthread
            dl
            m
    )
elseif(WIN32)
    # windows: depending on ffmpeg build, these are common
    list(APPEND MONOLITH_LIBS
            ws2_32
            secur32
            bcrypt
            avrt
    )
endif()
target_compile_features(monolith PRIVATE cxx_std_23)

target_link_libraries(monolith PRIVATE ${MONOLITH_LIBS})

# End of file
