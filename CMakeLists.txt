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
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(zlib_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/zlib")
set(zlib_BINARY_DIR "${CMAKE_BINARY_DIR}/third_party/zlib-build")
set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

add_subdirectory(${zlib_SOURCE_DIR} ${zlib_BINARY_DIR})

# libpng
set(PNG_SHARED OFF CACHE BOOL "" FORCE)
set(PNG_STATIC ON CACHE BOOL "" FORCE)
set(PNG_TESTS OFF CACHE BOOL "" FORCE)
set(libpng_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/libpng")
set(libpng_BINARY_DIR "${CMAKE_BINARY_DIR}/third_party/libpng-build")

add_subdirectory(${libpng_SOURCE_DIR} ${libpng_BINARY_DIR})

# libogg
set(libogg_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/libogg")
set(libogg_BINARY_DIR "${CMAKE_BINARY_DIR}/third_party/libogg-build")

add_subdirectory(${libogg_SOURCE_DIR} ${libogg_BINARY_DIR})

# libFLAC
set(BUILD_CXXLIBS OFF CACHE BOOL "" FORCE)
set(BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(INSTALL_MANPAGES OFF CACHE BOOL "" FORCE)
set(libflac_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/libflac")
set(libflac_BINARY_DIR "${CMAKE_BINARY_DIR}/third_party/libflac-build")

add_subdirectory(${libflac_SOURCE_DIR} ${libflac_BINARY_DIR})

# If the lib provides a 'man' target we exclude it from all (same semantics as before)
if(TARGET man)
    set_target_properties(man PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()

# wavpack
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE)
set(WAVPACK_BUILD_CLI OFF CACHE BOOL "Build command line programs" FORCE)
set(WAVPACK_BUILD_DOCS OFF CACHE BOOL "Build documentation" FORCE)
set(WAVPACK_BUILD_TESTS OFF CACHE BOOL "Build tests" FORCE)
set(wavpack_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/wavpack")
set(wavpack_BINARY_DIR "${CMAKE_BINARY_DIR}/third_party/wavpack-build")

add_subdirectory(${wavpack_SOURCE_DIR} ${wavpack_BINARY_DIR})

# libarchive
set(ENABLE_OPENSSL OFF CACHE BOOL "Disable OpenSSL in libarchive" FORCE)
set(ENABLE_TAR OFF CACHE BOOL "" FORCE)
set(ENABLE_CPIO OFF CACHE BOOL "" FORCE)
set(ENABLE_CAT OFF CACHE BOOL "" FORCE)
set(ENABLE_TEST OFF CACHE BOOL "" FORCE)
set(ENABLE_ZLIB ON CACHE BOOL "" FORCE)
set(ENABLE_MBEDTLS OFF CACHE BOOL "" FORCE)
set(ENABLE_NETTLE  OFF CACHE BOOL "" FORCE)
set(ENABLE_LIBXML2 OFF CACHE BOOL "" FORCE)
set(ENABLE_EXPAT   OFF CACHE BOOL "" FORCE)
set(ENABLE_PCREPOSIX  OFF CACHE BOOL "" FORCE)
set(ENABLE_PCRE2POSIX OFF CACHE BOOL "" FORCE)
set(ENABLE_LIBB2 OFF CACHE BOOL "" FORCE)
set(ENABLE_LZ4   OFF CACHE BOOL "" FORCE)
set(ENABLE_LZMA  OFF CACHE BOOL "" FORCE)
set(ENABLE_ZSTD  OFF CACHE BOOL "" FORCE)
set(libarchive_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/libarchive")
set(libarchive_BINARY_DIR "${CMAKE_BINARY_DIR}/third_party/libarchive-build")
add_subdirectory(${libarchive_SOURCE_DIR} ${libarchive_BINARY_DIR})

# mozjpeg
ExternalProject_Add(mozjpeg
        SOURCE_DIR ${CMAKE_SOURCE_DIR}/third_party/mozjpeg
        SOURCE_SUBDIR .
        PREFIX ${CMAKE_BINARY_DIR}/mozjpeg
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
set(libebml_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/libebml")
set(libebml_BINARY_DIR "${CMAKE_BINARY_DIR}/third_party/libebml-build")
add_subdirectory(${libebml_SOURCE_DIR} ${libebml_BINARY_DIR})

list(APPEND CMAKE_PREFIX_PATH "${CMAKE_SOURCE_DIR}/cmake")

# libMatroska
set(libmatroska_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/libmatroska")
set(libmatroska_BINARY_DIR "${CMAKE_BINARY_DIR}/third_party/libmatroska-build")

add_subdirectory(${libmatroska_SOURCE_DIR} ${libmatroska_BINARY_DIR})

# libwebp
set(WEBP_BUILD_ANIM_UTILS OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_CWEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_DWEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_GIF2WEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_IMG2WEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_VWEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_WEBPINFO OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_WEBPMUX OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_EXTRAS OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_TESTS OFF CACHE BOOL "" FORCE)

set(libwebp_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/libwebp")
set(libwebp_BINARY_DIR "${CMAKE_BINARY_DIR}/third_party/libwebp-build")
add_subdirectory(${libwebp_SOURCE_DIR} ${libwebp_BINARY_DIR})
add_library(thirdparty_webp ALIAS webp)
add_library(thirdparty_webpdecoder ALIAS webpdecoder)
add_library(thirdparty_webpdemux ALIAS webpdemux)
add_library(thirdparty_webpmux ALIAS libwebpmux)

# ffmpeg
set(FFMPEG_PREFIX ${CMAKE_BINARY_DIR}/ffmpeg-install)
ExternalProject_Add(ffmpeg
        SOURCE_DIR ${CMAKE_SOURCE_DIR}/third_party/ffmpeg
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
add_definitions(-DSKIP_OS_SECURE_RANDOM)
set(qpdf_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/qpdf")
set(qpdf_BINARY_DIR "${CMAKE_BINARY_DIR}/third_party/qpdf-build")
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(INSTALL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_DOC OFF CACHE BOOL "" FORCE)
set(QPDF_BUILD_TESTS OFF CACHE BOOL "Disable qpdf tests" FORCE)
set(QPDF_BUILD_EXAMPLES OFF CACHE BOOL "Disable qpdf examples" FORCE)
set(QPDF_BUILD_DOC OFF CACHE BOOL "Disable qpdf docs" FORCE)
set(QPDF_BUILD_FUZZ OFF CACHE BOOL "Disable qpdf fuzz targets" FORCE)
set(QPDF_INSTALL OFF CACHE BOOL "Disable qpdf install rules" FORCE)
set(SKIP_INSTALL_ALL ON CACHE BOOL "Skip all install rules" FORCE)
set(QPDF_USE_CRYPTO_NATIVE ON CACHE BOOL "Use qpdf's built-in crypto" FORCE)
set(QPDF_USE_CRYPTO_OPENSSL OFF CACHE BOOL "Disable OpenSSL crypto" FORCE)
set(QPDF_USE_CRYPTO_GNUTLS OFF CACHE BOOL "Disable GnuTLS crypto" FORCE)
set(REQUIRE_CRYPTO_NATIVE ON CACHE BOOL "" FORCE)
set(USE_IMPLICIT_CRYPTO OFF CACHE BOOL "" FORCE)
set(INSTALL_CMAKE_PACKAGE OFF CACHE BOOL "Disable qpdf CMake package export" FORCE)
set(INSTALL_PKGCONFIG OFF CACHE BOOL "Disable qpdf pkg-config export" FORCE)
if (NOT WIN32)
    find_package(qpdf REQUIRED)
endif()
add_subdirectory(${qpdf_SOURCE_DIR} ${qpdf_BINARY_DIR})

# zopfli
set(zopfli_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/zopfli")
set(zopfli_BINARY_DIR "${CMAKE_BINARY_DIR}/third_party/zopfli-build")
add_subdirectory(${zopfli_SOURCE_DIR} ${zopfli_BINARY_DIR})

# libjxl
set(JPEGXL_ENABLE_TOOLS OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_DEVTOOLS OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_PLUGINS OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_MANPAGES OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_BENCHMARK OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(libjxl_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/libjxl")
set(libjxl_BINARY_DIR "${CMAKE_BINARY_DIR}/third_party/libjxl-build")
add_subdirectory(${libjxl_SOURCE_DIR} ${libjxl_BINARY_DIR})

# libtiff
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(tiff-tools OFF CACHE BOOL "" FORCE)
set(tiff-tests OFF CACHE BOOL "" FORCE)
set(tiff-docs OFF CACHE BOOL "" FORCE)
set(libtiff_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/libtiff")
set(libtiff_BINARY_DIR "${CMAKE_BINARY_DIR}/third_party/libtiff-build")
add_subdirectory(${libtiff_SOURCE_DIR} ${libtiff_BINARY_DIR})

# sqlite3
set(sqlite3_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/sqlite3")
set(sqlite3_BINARY_DIR "${CMAKE_BINARY_DIR}/third_party/sqlite3-build")

add_subdirectory(${sqlite3_SOURCE_DIR} ${sqlite3_BINARY_DIR})

# libmseed
set(libmseed_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/libmseed")
set(libmseed_BINARY_DIR "${CMAKE_BINARY_DIR}/libmseed-build")
set(libmseed_INSTALL_DIR "${CMAKE_BINARY_DIR}/libmseed-install")

ExternalProject_Add(libmseed
        SOURCE_DIR ${libmseed_SOURCE_DIR}
        CONFIGURE_COMMAND ""
        BUILD_COMMAND make
        INSTALL_COMMAND make install PREFIX=${libmseed_INSTALL_DIR}
        BUILD_IN_SOURCE 1
        BUILD_BYPRODUCTS ${CMAKE_SOURCE_DIR}/third_party/libmseed/libmseed.a
)

# gifsicle

configure_file(
        ${CMAKE_SOURCE_DIR}/third_party/gifsicle/src/win32cfg.h
        ${CMAKE_SOURCE_DIR}/third_party/gifsicle/src/config.h
        COPYONLY
)
add_library(giflib STATIC
        third_party/gifsicle/src/clp.c
        third_party/gifsicle/src/fmalloc.c
        third_party/gifsicle/src/gifdiff.c
        third_party/gifsicle/src/giffunc.c
        third_party/gifsicle/src/gifread.c
        # third_party/gifsicle/src/gifsicle.c
        third_party/gifsicle/src/giftoc.c
        third_party/gifsicle/src/gifunopt.c
        # third_party/gifsicle/src/gifview.c
        third_party/gifsicle/src/gifwrite.c
        third_party/gifsicle/src/kcolor.c
        third_party/gifsicle/src/merge.c
        third_party/gifsicle/src/optimize.c
        # third_party/gifsicle/src/opttemplate.c
        third_party/gifsicle/src/quantize.c
        third_party/gifsicle/src/strerror.c
        third_party/gifsicle/src/support.c
        third_party/gifsicle/src/ungifwrt.c
)

target_include_directories(giflib PUBLIC
        third_party/gifsicle/include
        third_party/gifsicle/src
)

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
        src/encoder/mseed_encoder.cpp
        src/encoder/mseed_encoder.hpp
        src/containers/odf_handler.cpp
        src/containers/odf_handler.hpp
        src/containers/ooxml_handler.cpp
        src/containers/ooxml_handler.hpp
        src/utils/random_utils.cpp
        src/utils/random_utils.hpp
        src/encoder/gif_encoder.cpp
        src/encoder/gif_encoder.hpp
)

add_dependencies(monolith libmseed)
target_include_directories(monolith PRIVATE ${libmseed_SOURCE_DIR})

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
        giflib
        png_static
        archive_static
        ogg
        ${libmseed_SOURCE_DIR}/libmseed.a
        ${ZLIB_TARGET}
        ebml
        matroska
        wavpack
        thirdparty_webp
        thirdparty_webpdecoder
        thirdparty_webpdemux
        thirdparty_webpmux
        ffmpeg_avcodec
        ffmpeg_avformat
        ffmpeg_avutil
        libqpdf
        libzopfli
        libzopflipng
        jxl
        jxl_threads
        tiff
        SQLite::SQLite3
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
