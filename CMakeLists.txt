cmake_minimum_required(VERSION 3.5...4.1.2)

project(chisel VERSION 1.9.0 LANGUAGES CXX C)

if(MSVC)
    # force static msvc runtime linkage (/mt for release, /mtd for debug)
    if(POLICY CMP0091)
        cmake_policy(SET CMP0091 NEW)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endif()

# prevents redefinition of 'uninstall' target
macro(add_custom_target target_name)
    if("${target_name}" STREQUAL "uninstall")
        if(NOT TARGET uninstall)
            _add_custom_target(${ARGV})
        endif()
    else()
        _add_custom_target(${ARGV})
    endif()
endmacro()

option(CHISEL_BUILD_CLI "Build the chisel command-line executable" ON)
option(ENABLE_OPTIVORBIS "Enable rust optivorbis bridge" ON)
option(ENABLE_MATROSKA "Enable LibEBML and LibMatroska integration" ON)

if(CMAKE_BUILD_TYPE STREQUAL "Release" AND NOT DEFINED CMAKE_INTERPROCEDURAL_OPTIMIZATION)
    set(CMAKE_UNITY_BUILD TRUE)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
    if(ipo_supported)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
    else()
        message(WARNING "IPO/LTO is not supported: ${ipo_error}")
    endif()
else()
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION FALSE)
endif()

if (CMAKE_VERSION VERSION_GREATER_EQUAL "3.30")
    set(CMAKE_POLICY_VERSION_MINIMUM "3.5")
endif()
if (WIN32 OR NOT APPLE)
    add_compile_definitions(HAVE_UINTPTR_T=1 HAVE_STDINT_H=1 HAVE_INTTYPES_H=1)
endif()
cmake_policy(SET CMP0069 NEW)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

include(CheckCCompilerFlag)
include(CheckCXXCompilerFlag)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    if (MSVC)
        add_compile_options(/Od /Zi /W0)
    else()
        add_compile_options(-O0 -g3 -fno-omit-frame-pointer -w)
        add_compile_options(-fsanitize=address,undefined)
        add_link_options(-fsanitize=address,undefined)
    endif()
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/debug)

elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
    if (MSVC)
        add_compile_options(/O2 /GL /W0)
        add_link_options(/OPT:REF /OPT:ICF) # dead code elimination on windows
    else()
        add_compile_options(-O3 -w)

        # place each function and data in its own section to allow linker to strip them
        add_compile_options(-ffunction-sections -fdata-sections)

        if(APPLE)
            # macos linker flag to remove unused sections
            add_link_options("-Wl,-dead_strip")
        else()
            # linux/mingw linker flags to remove unused sections and strip symbols
            add_link_options("-Wl,--gc-sections" "-s")
        endif()
    endif()
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/release)
endif()

include(FetchContent)
include(ExternalProject)

# bzip2
set(BZIP2_DIR "${CMAKE_SOURCE_DIR}/third_party/bzip2")

if(EXISTS "${BZIP2_DIR}/bzlib.c")
    set(BZ_VERSION "1.1.0")
    configure_file(
            "${BZIP2_DIR}/bz_version.h.in"
            "${CMAKE_CURRENT_BINARY_DIR}/bzip2_inc/bz_version.h"
    )

    add_library(bz2_static STATIC EXCLUDE_FROM_ALL
            ${BZIP2_DIR}/blocksort.c
            ${BZIP2_DIR}/huffman.c
            ${BZIP2_DIR}/crctable.c
            ${BZIP2_DIR}/randtable.c
            ${BZIP2_DIR}/compress.c
            ${BZIP2_DIR}/decompress.c
            ${BZIP2_DIR}/bzlib.c
    )

    target_include_directories(bz2_static PUBLIC
            ${BZIP2_DIR}
            "${CMAKE_CURRENT_BINARY_DIR}/bzip2_inc"
    )

    if(MSVC)
        target_compile_definitions(bz2_static PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()

    set(BZIP2_INCLUDE_DIR "${BZIP2_DIR}" CACHE PATH "" FORCE)
    set(BZIP2_LIBRARIES bz2_static CACHE STRING "" FORCE)
    set(BZIP2_FOUND TRUE CACHE BOOL "" FORCE)

    set(BZIP2_LIBRARY_RELEASE bz2_static CACHE STRING "" FORCE)
    set(BZIP2_LIBRARY bz2_static CACHE STRING "" FORCE)
endif()

# libmkclean
if(ENABLE_MATROSKA)
    add_subdirectory("third_party/libmkclean" EXCLUDE_FROM_ALL)
endif()

# liblzma
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(XZ_NLS OFF CACHE BOOL "Turned off XZ translated man pages" FORCE)
set(XZ_DOXYGEN OFF CACHE BOOL "Turned off XZ doxygen" FORCE)
set(XZ_DOC OFF CACHE BOOL "Turned off XZ docs" FORCE)
set(xz_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/xz")
set(xz_BINARY_DIR "${CMAKE_BINARY_DIR}/third_party/xz-build")
set(ENABLE_NLS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_DOC OFF CACHE BOOL "" FORCE)
set(BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_XZDEC OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_LZMADEC OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_LZMAINFO OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_XZ OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_SYMLINKS OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_SYMLINKS_LZMA OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_SCRIPTS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/xz ${CMAKE_BINARY_DIR}/xz-build EXCLUDE_FROM_ALL)
set(LIBLZMA_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/third_party/xz/src/liblzma/api" CACHE PATH "liblzma include" FORCE)

# lz4
set(LZ4_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(LZ4_BUILD_LEGACY_LZ4C OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/lz4/build/cmake EXCLUDE_FROM_ALL)

# zstd
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED   OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_DEPRECATED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_DICTBUILDER OFF CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
set(zstd_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/zstd/build/cmake")
set(zstd_BINARY_DIR "${CMAKE_BINARY_DIR}/third_party/zstd-build")
add_subdirectory(${zstd_SOURCE_DIR} ${zstd_BINARY_DIR} EXCLUDE_FROM_ALL)
set(ZSTD_FOUND TRUE CACHE BOOL "" FORCE)
set(ZSTD_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/third_party/zstd/lib" CACHE PATH "" FORCE)
set(ZSTD_INCLUDE_DIRS "${ZSTD_INCLUDE_DIR}" CACHE PATH "" FORCE)
set(ZSTD_LIBRARY libzstd_static CACHE STRING "" FORCE)
set(ZSTD_LIBRARIES libzstd_static CACHE STRING "" FORCE)

# libdeflate
set(LIBDEFLATE_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_GZIP OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(libdeflate_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/libdeflate")
set(libdeflate_BINARY_DIR "${CMAKE_BINARY_DIR}/third_party/libdeflate-build")
add_subdirectory(${libdeflate_SOURCE_DIR} ${libdeflate_BINARY_DIR} EXCLUDE_FROM_ALL)

# force libtiff to use our libdeflate
set(DEFLATE_FOUND TRUE CACHE BOOL "" FORCE)
set(DEFLATE_LIBRARY libdeflate_static CACHE STRING "" FORCE)
set(DEFLATE_INCLUDE_DIR "${libdeflate_SOURCE_DIR}" CACHE PATH "" FORCE)

# qadmimes
add_subdirectory(third_party/qadmimes EXCLUDE_FROM_ALL)

# zlib
set(ZLIB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZLIB_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZLIB_BUILD_TESTING OFF CACHE BOOL "" FORCE)

set(zlib_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/zlib")
set(zlib_BINARY_DIR "${CMAKE_BINARY_DIR}/third_party/zlib-build")
add_subdirectory(${zlib_SOURCE_DIR} ${zlib_BINARY_DIR} EXCLUDE_FROM_ALL)

if(NOT TARGET zlib)
    add_library(zlib ALIAS zlibstatic)
endif()

if(NOT TARGET ZLIB::ZLIB)
    add_library(ZLIB::ZLIB ALIAS zlibstatic)
endif()
set(ZLIB_H_PATH "${zlib_SOURCE_DIR};${zlib_BINARY_DIR}" CACHE STRING "" FORCE)
set(ZLIB_LIB_PATH ZLIB::ZLIB CACHE STRING "" FORCE)
set(ZLIB_INCLUDE_DIR "${zlib_SOURCE_DIR};${zlib_BINARY_DIR}" CACHE PATH "zlib include dir" FORCE)
set(ZLIB_INCLUDE_DIRS "${ZLIB_INCLUDE_DIR}" CACHE PATH "zlib include dirs" FORCE)

set(ZLIB_LIBRARY zlibstatic CACHE STRING "zlib library" FORCE)
set(ZLIB_LIBRARIES zlibstatic CACHE STRING "zlib libraries" FORCE)
set(ZLIB_FOUND TRUE CACHE BOOL "zlib found" FORCE)

set(ENABLE_STATIC_RUNTIME OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_BINDINGS OFF CACHE BOOL "" FORCE)
set(VISIBILITY_HIDDEN ON CACHE BOOL "" FORCE)
set(taglib_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/taglib")
set(taglib_BINARY_DIR "${CMAKE_BINARY_DIR}/third_party/taglib-build")
add_subdirectory(${taglib_SOURCE_DIR} ${taglib_BINARY_DIR} EXCLUDE_FROM_ALL)
list(APPEND taglib_SOURCE_DIR
        "${CMAKE_SOURCE_DIR}/third_party/taglib/taglib"
        "${CMAKE_SOURCE_DIR}/third_party/taglib/taglib/toolkit"
        "${CMAKE_SOURCE_DIR}/third_party/taglib/taglib/flac"
        "${CMAKE_SOURCE_DIR}/third_party/taglib/taglib/ogg"
        "${CMAKE_SOURCE_DIR}/third_party/taglib/taglib/mpeg/id3v2"
        "${CMAKE_SOURCE_DIR}/third_party/taglib/taglib/riff"
)
# libpng
set(PNG_SHARED OFF CACHE BOOL "" FORCE)
set(PNG_STATIC ON CACHE BOOL "" FORCE)
set(PNG_TESTS OFF CACHE BOOL "" FORCE)
set(PNG_TOOLS OFF CACHE BOOL "" FORCE)
set(PNG_EXECUTABLES OFF CACHE BOOL "" FORCE)
set(PNG_DEBUG OFF CACHE BOOL "" FORCE)
set(libpng_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/libpng")
set(libpng_BINARY_DIR "${CMAKE_BINARY_DIR}/third_party/libpng-build")
add_subdirectory(${libpng_SOURCE_DIR} ${libpng_BINARY_DIR} EXCLUDE_FROM_ALL)

# pugixml
add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/pugixml EXCLUDE_FROM_ALL)

# libogg
set(INSTALL_DOCS OFF CACHE BOOL "" FORCE)
set(INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(INSTALL_CMAKE_PACKAGE_MODULE OFF CACHE BOOL "" FORCE)
set(libogg_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/libogg")
set(libogg_BINARY_DIR "${CMAKE_BINARY_DIR}/third_party/libogg-build")
add_subdirectory(${libogg_SOURCE_DIR} ${libogg_BINARY_DIR} EXCLUDE_FROM_ALL)

# libFLAC
set(BUILD_CXXLIBS OFF CACHE BOOL "" FORCE)
set(BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(INSTALL_MANPAGES OFF CACHE BOOL "" FORCE)
set(BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(INSTALL_PKGCONFIG_MODULES OFF CACHE BOOL "" FORCE)
set(INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(libflac_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/libflac")
set(libflac_BINARY_DIR "${CMAKE_BINARY_DIR}/third_party/libflac-build")
add_subdirectory(${libflac_SOURCE_DIR} ${libflac_BINARY_DIR} EXCLUDE_FROM_ALL)
if(TARGET man)
    set_target_properties(man PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()

# flacoutcpp: reuses the FLAC target just defined above instead of vendoring
# its own copy (see the if(NOT TARGET FLAC) guard in its own CMakeLists.txt)
set(FLACOUT_NATIVE OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/flacoutcpp EXCLUDE_FROM_ALL)

# flexiGIF
add_subdirectory(third_party/flexigif EXCLUDE_FROM_ALL)

# wavpack
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE)
set(WAVPACK_BUILD_CLI OFF CACHE BOOL "Build command line programs" FORCE)
set(WAVPACK_BUILD_DOCS OFF CACHE BOOL "Build documentation" FORCE)
set(WAVPACK_BUILD_TESTS OFF CACHE BOOL "Build tests" FORCE)
set(WAVPACK_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(WAVPACK_INSTALL_CMAKE_MODULE OFF CACHE BOOL "" FORCE)
set(WAVPACK_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
set(WAVPACK_INSTALL_PKGCONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(wavpack_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/wavpack")
set(wavpack_BINARY_DIR "${CMAKE_BINARY_DIR}/third_party/wavpack-build")
add_subdirectory(${wavpack_SOURCE_DIR} ${wavpack_BINARY_DIR} EXCLUDE_FROM_ALL)

# libarchive
# tvOS and watchOS lack support for spawning child processes and external pipes
if(CMAKE_SYSTEM_NAME MATCHES "^(tvOS|watchOS)$")
    set(HAVE_POSIX_SPAWN  FALSE CACHE INTERNAL "")
    set(HAVE_POSIX_SPAWNP FALSE CACHE INTERNAL "")
    set(HAVE_FORK         FALSE CACHE INTERNAL "")
    set(HAVE_VFORK        FALSE CACHE INTERNAL "")
    set(HAVE_PIPE         FALSE CACHE INTERNAL "")

    add_compile_definitions(HAVE_PIPE=0)
endif()
set(ENABLE_OPENSSL OFF CACHE BOOL "Disable OpenSSL in libarchive" FORCE)
set(ENABLE_TAR ON CACHE BOOL "" FORCE)
set(ENABLE_CPIO ON CACHE BOOL "" FORCE)
set(ENABLE_CAT OFF CACHE BOOL "" FORCE)
set(ENABLE_TEST OFF CACHE BOOL "" FORCE)
set(ENABLE_MBEDTLS OFF CACHE BOOL "" FORCE)
set(ENABLE_NETTLE  OFF CACHE BOOL "" FORCE)
set(ENABLE_LIBXML2 OFF CACHE BOOL "" FORCE)
set(ENABLE_EXPAT   OFF CACHE BOOL "" FORCE)
set(ENABLE_LZ4 ON CACHE BOOL "" FORCE)
set(ENABLE_BZip2 ON CACHE BOOL "" FORCE)
set(ENABLE_LIBB2 ON CACHE BOOL "" FORCE)
set(ENABLE_PCREPOSIX  OFF CACHE BOOL "" FORCE)
set(ENABLE_PCRE2POSIX OFF CACHE BOOL "" FORCE)
set(ENABLE_SHARED OFF CACHE BOOL "" FORCE)
set(ENABLE_STATIC ON CACHE BOOL "" FORCE)
set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(ENABLE_TAR_SHARED OFF CACHE BOOL "" FORCE)
set(ENABLE_CPIO_SHARED OFF CACHE BOOL "" FORCE)
set(ENABLE_CAT_SHARED OFF CACHE BOOL "" FORCE)
set(ENABLE_UNZIP_SHARED OFF CACHE BOOL "" FORCE)
set(ENABLE_COVERAGE OFF CACHE BOOL "" FORCE)
set(libarchive_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/libarchive")
set(libarchive_BINARY_DIR "${CMAKE_BINARY_DIR}/third_party/libarchive-build")
# set(HAVE_LIBLZMA 1 CACHE INTERNAL "Force liblzma found")
set(LZ4_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/third_party/lz4/lib" CACHE PATH "LZ4 header dir" FORCE)
set(LZ4_LIBRARY lz4_static CACHE FILEPATH "LZ4 static library" FORCE)
set(LZ4_FOUND TRUE CACHE BOOL "Force LZ4 found" FORCE)
set(HAVE_LIBLZ4 1 CACHE INTERNAL "Force liblz4 macros")
set(HAVE_LZ4_H 1 CACHE INTERNAL "Force liblz4 macros")

# liblzma
add_subdirectory(${libarchive_SOURCE_DIR} ${libarchive_BINARY_DIR} EXCLUDE_FROM_ALL)
target_include_directories(archive_static PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party/xz/src/liblzma/api
)
add_dependencies(archive_static liblzma)
target_compile_definitions(archive_static PRIVATE LZMA_API_STATIC)
set(BZIP2_INCLUDE_DIR "${BZIP2_DIR}" CACHE PATH "" FORCE)
set(BZIP2_LIBRARY_RELEASE bz2_static CACHE STRING "" FORCE)
set(BZIP2_LIBRARY bz2_static CACHE STRING "" FORCE)
set(BZIP2_LIBRARIES bz2_static CACHE STRING "" FORCE)

set(LIBLZMA_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/third_party/xz/src/liblzma/api" CACHE PATH "" FORCE)
set(LIBLZMA_LIBRARY liblzma CACHE STRING "" FORCE)
set(LIBLZMA_LIBRARIES liblzma CACHE STRING "" FORCE)

# bmplib
set(insanity_limit_mb 1024 CACHE STRING "Insanity limit for bmp in MBs" FORCE)
set(BMPLIB_GEN_HUFFMAN OFF)
add_subdirectory(third_party/bmplib EXCLUDE_FROM_ALL)

# --- mozjpeg via ExternalProject ---
if(MSVC)
    set(_mozjpeg_lib "${CMAKE_BINARY_DIR}/mozjpeg/src/mozjpeg-build/Release/jpeg-static.lib")
else()
    set(_mozjpeg_lib "${CMAKE_BINARY_DIR}/mozjpeg/src/mozjpeg-build/libjpeg.a")
endif()

set(MOZJPEG_CROSS_ARGS "")

if(APPLE)
    if(CMAKE_OSX_ARCHITECTURES)
        list(GET CMAKE_OSX_ARCHITECTURES 0 CURRENT_ARCH)
    else()
        set(CURRENT_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
    endif()
    list(APPEND MOZJPEG_CROSS_ARGS
            "-DCMAKE_SYSTEM_NAME=${CMAKE_SYSTEM_NAME}"
            "-DCMAKE_OSX_SYSROOT=${CMAKE_OSX_SYSROOT}"
            "-DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}"
            "-DCMAKE_SYSTEM_PROCESSOR=${CURRENT_ARCH}"
    )
endif()

ExternalProject_Add(mozjpeg
        SOURCE_DIR ${CMAKE_SOURCE_DIR}/third_party/mozjpeg
        SOURCE_SUBDIR .
        PREFIX ${CMAKE_BINARY_DIR}/mozjpeg
        CMAKE_ARGS
        -DENABLE_SHARED=FALSE
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
        -DCMAKE_INSTALL_PREFIX=${CMAKE_BINARY_DIR}/mozjpeg-install
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5
        -DPNG_SUPPORTED=0
        -DWITH_FUZZ=0
        -DWITH_JAVA=0
        -DWITH_TURBOJPEG=0
        ${MOZJPEG_CROSS_ARGS}
        -DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}
        -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
        -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        # ExternalProject_Add does NOT inherit the parent project's compiler/linker
        # flags automatically; forward them explicitly so cross-compilation flags
        # (e.g. -m32) reach mozjpeg's own CMake configure and its CPU_TYPE/BITS
        # detection, which otherwise defaults to the host's native architecture.
        -DCMAKE_C_FLAGS=${CMAKE_C_FLAGS}
        -DCMAKE_CXX_FLAGS=${CMAKE_CXX_FLAGS}
        -DCMAKE_EXE_LINKER_FLAGS=${CMAKE_EXE_LINKER_FLAGS}
        -DCMAKE_SHARED_LINKER_FLAGS=${CMAKE_SHARED_LINKER_FLAGS}
        UPDATE_DISCONNECTED 1
        BUILD_BYPRODUCTS ${_mozjpeg_lib}
)

set(_mozjpeg_build_dir "${CMAKE_BINARY_DIR}/mozjpeg/src/mozjpeg-build")
set(_mozjpeg_jconfig   "${_mozjpeg_build_dir}/jconfig.h")
set(_mozjpeg_dest      "${CMAKE_SOURCE_DIR}/third_party/mozjpeg/jconfig.h")

add_custom_target(copy_mozjpeg_jconfig ALL
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_mozjpeg_jconfig}" "${_mozjpeg_dest}"
        DEPENDS mozjpeg
        COMMENT "Copying mozjpeg jconfig.h into third_party/mozjpeg"
)

add_library(mozjpeg_static STATIC IMPORTED GLOBAL)
set_target_properties(mozjpeg_static PROPERTIES
        IMPORTED_LOCATION "${_mozjpeg_lib}"
        INTERFACE_INCLUDE_DIRECTORIES
        "${CMAKE_SOURCE_DIR}/third_party/mozjpeg;${CMAKE_BINARY_DIR}/mozjpeg/src/mozjpeg-build"
)
add_dependencies(mozjpeg_static mozjpeg copy_mozjpeg_jconfig)
remove_definitions(JCONFIG_INCLUDED)
add_library(JPEG::JPEG ALIAS mozjpeg_static)

set(JPEG_INCLUDE "${CMAKE_SOURCE_DIR}/third_party/mozjpeg" CACHE PATH "jpeg include dir")
set(JPEG_INCLUDE_DIR "${JPEG_INCLUDE}" CACHE PATH "jpeg include dir")
set(LIBJPEG_H_PATH "${JPEG_INCLUDE}" CACHE PATH "jpeg include dir")
set(LIBJPEG_LIB_PATH "${_mozjpeg_lib}" CACHE FILEPATH "jpeg library")
set(JPEG_FOUND TRUE CACHE BOOL "jpeg found")

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
set(WEBP_BUILD_WEBP_JS OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_LIBWEBPMUX ON CACHE BOOL "" FORCE)
set(libwebp_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/libwebp")
set(libwebp_BINARY_DIR "${CMAKE_BINARY_DIR}/third_party/libwebp-build")
add_subdirectory(${libwebp_SOURCE_DIR} ${libwebp_BINARY_DIR} EXCLUDE_FROM_ALL)
add_library(thirdparty_webp ALIAS webp)
add_library(thirdparty_webpdemux ALIAS webpdemux)
add_library(thirdparty_webpmux ALIAS libwebpmux)

# qpdf
add_definitions(-DSKIP_OS_SECURE_RANDOM)
set(ZOPFLI OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(qpdf_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/qpdf")
set(qpdf_BINARY_DIR "${CMAKE_BINARY_DIR}/third_party/qpdf-build")
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(INSTALL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_DOC OFF CACHE BOOL "" FORCE)
set(REQUIRE_CRYPTO_NATIVE ON CACHE BOOL "" FORCE)
set(USE_IMPLICIT_CRYPTO OFF CACHE BOOL "" FORCE)
set(INSTALL_CMAKE_PACKAGE OFF CACHE BOOL "Disable qpdf CMake package export" FORCE)
set(INSTALL_PKGCONFIG OFF CACHE BOOL "Disable qpdf pkg-config export" FORCE)
set(INSTALL_MANUAL OFF CACHE BOOL "" FORCE)
add_subdirectory(${qpdf_SOURCE_DIR} ${qpdf_BINARY_DIR} EXCLUDE_FROM_ALL)
if(TARGET libqpdf_object)
    add_dependencies(libqpdf_object copy_mozjpeg_jconfig)
endif()
if(TARGET libqpdf)
    add_dependencies(libqpdf copy_mozjpeg_jconfig)
endif()

# zopfli
set(ZOPFLI_BUILD_INSTALL OFF CACHE BOOL "" FORCE)
set(zopfli_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/zopfli")
set(zopfli_BINARY_DIR "${CMAKE_BINARY_DIR}/third_party/zopfli-build")
add_subdirectory(${zopfli_SOURCE_DIR} ${zopfli_BINARY_DIR} EXCLUDE_FROM_ALL)

# libjxl
set(HWY_ENABLE_CONTRIB OFF CACHE BOOL "" FORCE)
set(HWY_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(HWY_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(HWY_COMPILE_ONLY_STATIC ON CACHE BOOL "" FORCE)
set(HWY_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BROTLI_DISABLE_TESTS ON CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_FUZZERS OFF CACHE BOOL "" FORCE)
set(JPEGXL_TEST_TOOLS OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_PLUGIN_GDKPIXBUF OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_PLUGIN_GIMP210 OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_PLUGIN_MIME OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_DOXYGEN OFF CACHE BOOL "Turned off JXL doxygen" FORCE)
set(JPEGXL_ENABLE_TOOLS OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_DEVTOOLS OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_PLUGINS OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_MANPAGES OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_BENCHMARK OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_JPEGLI OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_JPEGLI_LIBJPEG OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_SJPEG OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_OPENEXR OFF CACHE BOOL "" FORCE)
set(JPEGXL_BUNDLE_LIBPNG OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_JNI OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_VIEWERS OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_TCMALLOC OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_TRANSCODE_JPEG OFF CACHE BOOL "" FORCE)
set(libjxl_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/libjxl")
set(libjxl_BINARY_DIR "${CMAKE_BINARY_DIR}/third_party/libjxl-build")
add_subdirectory(${libjxl_SOURCE_DIR} ${libjxl_BINARY_DIR} EXCLUDE_FROM_ALL)

# libtiff
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(tiff-framework OFF CACHE BOOL "" FORCE)
set(tiff-deprecated OFF CACHE BOOL "" FORCE)
set(tiff-install OFF CACHE BOOL "" FORCE)
set(tiff-tools OFF CACHE BOOL "" FORCE)
set(tiff-tests OFF CACHE BOOL "" FORCE)
set(tiff-docs OFF CACHE BOOL "" FORCE)
set(tiff-contrib OFF CACHE BOOL "" FORCE)
set(cxx OFF CACHE BOOL "" FORCE)
set(tiff-opengl OFF CACHE BOOL "" FORCE)
set(jbig OFF CACHE BOOL "" FORCE)
set(lerc OFF CACHE BOOL "" FORCE)
set(webp OFF CACHE BOOL "" FORCE)
set(zstd OFF CACHE BOOL "" FORCE)
set(lzma OFF CACHE BOOL "" FORCE)
set(libdeflate OFF CACHE BOOL "" FORCE)
set(libtiff_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/libtiff")
set(libtiff_BINARY_DIR "${CMAKE_BINARY_DIR}/third_party/libtiff-build")
add_subdirectory(${libtiff_SOURCE_DIR} ${libtiff_BINARY_DIR} EXCLUDE_FROM_ALL)

# sqlite3
set(ENABLE_SHARED OFF CACHE BOOL "" FORCE)
set(BUILD_SHELL   OFF CACHE BOOL "" FORCE)
set(ENABLE_STATIC ON  CACHE BOOL "" FORCE)
set(ENABLE_STATIC_SHELL OFF CACHE BOOL "" FORCE)
set(ENABLE_READLINE OFF CACHE BOOL "" FORCE)
set(ENABLE_EDITLINE OFF CACHE BOOL "" FORCE)
if(CMAKE_SYSTEM_NAME MATCHES "^(iOS|tvOS|watchOS|visionOS)$")
    set(IOS TRUE CACHE INTERNAL "Disable gethostuuid in sqlite3")
endif()
set(sqlite3_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/sqlite3")
set(sqlite3_BINARY_DIR "${CMAKE_BINARY_DIR}/third_party/sqlite3-build")
add_subdirectory(${sqlite3_SOURCE_DIR} ${sqlite3_BINARY_DIR} EXCLUDE_FROM_ALL)

# mseedout
add_subdirectory(third_party/mseedout EXCLUDE_FROM_ALL)

# gifsicle
set(BUILD_GIFSICLE_CLI OFF CACHE BOOL "" FORCE)
set(BUILD_GIFDIFF OFF CACHE BOOL "" FORCE)
set(BUILD_GIFVIEW OFF CACHE BOOL "" FORCE)

add_subdirectory(third_party/gifsicle EXCLUDE_FROM_ALL)

# Monkey's Audio
set(BUILD_UTIL OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/MACLib EXCLUDE_FROM_ALL)

# zopfli include hint for targets (safe to keep)
target_include_directories(zopflipng PUBLIC
        ${zopfli_SOURCE_DIR}/src/zopflipng
        ${zopfli_SOURCE_DIR}/src/zopfli
)

# mp3packercpp
set(USE_OPENMP OFF CACHE BOOL "Enable OpenMP for mp3packercpp" FORCE)
add_subdirectory(third_party/mp3packercpp EXCLUDE_FROM_ALL)

# brotli
set(BROTLIDEC_FOUND TRUE CACHE BOOL "" FORCE)
set(BROTLIENC_FOUND TRUE CACHE BOOL "" FORCE)
set(BROTLICOMMON_FOUND TRUE CACHE BOOL "" FORCE)

set(BROTLIDEC_LIBRARIES brotlidec CACHE STRING "" FORCE)
set(BROTLIENC_LIBRARIES brotlienc CACHE STRING "" FORCE)
set(BROTLICOMMON_LIBRARIES brotlicommon CACHE STRING "" FORCE)

set(BROTLIDEC_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/third_party/libjxl/third_party/brotli/c/include" CACHE PATH "" FORCE)
set(BROTLIENC_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/third_party/libjxl/third_party/brotli/c/include" CACHE PATH "" FORCE)
set(BROTLICOMMON_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/third_party/libjxl/third_party/brotli/c/include" CACHE PATH "" FORCE)

# disable cmake find_package for brotli to prevent homebrew leakage
set(CMAKE_DISABLE_FIND_PACKAGE_BrotliDec ON CACHE BOOL "" FORCE)
set(CMAKE_DISABLE_FIND_PACKAGE_BrotliEnc ON CACHE BOOL "" FORCE)

# woff2
set(WOFF2_SKIP_TESTS ON CACHE BOOL "" FORCE)
set(NOISY_LOGGING OFF)
if(APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(CANONICAL_PREFIXES ON CACHE BOOL "" FORCE)
endif ()
add_subdirectory(third_party/woff2 EXCLUDE_FROM_ALL)

# kanzicpp
set(CONCURRENCY_DISABLED ON CACHE BOOL "Disable multithreading in Kanzi" FORCE)
set(KANZI_ENABLE_NATIVE_OPTIMIZATIONS OFF CACHE BOOL "Disable native optimizations for portability" FORCE)
add_subdirectory(third_party/kanzi-cpp EXCLUDE_FROM_ALL)

# openjpeg
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBS ON  CACHE BOOL "" FORCE)
set(BUILD_CODEC OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_JPIP OFF CACHE BOOL "" FORCE)
set(BUILD_JPIP_SERVER OFF CACHE BOOL "" FORCE)
set(BUILD_VIEWER OFF CACHE BOOL "" FORCE)
set(BUILD_JAVA OFF CACHE BOOL "" FORCE)
set(BUILD_THIRDPARTY OFF CACHE BOOL "" FORCE)
set(BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_LUTS_GENERATOR OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/openjpeg EXCLUDE_FROM_ALL)

# common libraries for all platforms (exposed to libchisel/chisel_cli)
set(CHISEL_LIBS
        FLAC
        flacout_lib
        MAC
        png_static
        archive_static
        ogg
        mozjpeg_static
        wavpack
        flexigif_core
        thirdparty_webp
        thirdparty_webpdemux
        thirdparty_webpmux
        libqpdf
        libzopfli
        libzopflipng
        jxl
        jxl_threads
        tiff
        libdeflate_static
        sqlite3-static
        zlibstatic
        tag
        libgifsicle
        pugixml
        mseedout_lib
        mseed_static
        bmplib
        brotlicommon
        brotlidec
        brotlienc
        woff2common
        woff2dec
        woff2enc
        liblzma
        lz4_static
        libkanzi
        mp3packercpp_core
        openjp2
        qadmimes-core
)

if(ENABLE_MATROSKA)
    list(APPEND CHISEL_LIBS mkclean_lib)
    add_compile_definitions(HAVE_MATROSKA=1)
endif()

if(WIN32)
    list(APPEND CHISEL_LIBS
            Rpcrt4
    )
endif()

if(ENABLE_OPTIVORBIS)
    add_subdirectory(third_party/corrosion EXCLUDE_FROM_ALL)
    add_compile_definitions(HAVE_OPTIVORBIS=1)
endif()

add_subdirectory(libchisel)
add_dependencies(libchisel copy_mozjpeg_jconfig)

target_include_directories(libchisel SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/third_party/gifsicle/include"
        "${CMAKE_SOURCE_DIR}/third_party/gifsicle/src"
        "${CMAKE_SOURCE_DIR}/third_party/mseedout/include"
        "${CMAKE_SOURCE_DIR}/third_party/mseedout/third_party/libmseed"
)

if(CHISEL_BUILD_CLI)
    add_subdirectory(chisel_cli)
endif()

set(NON_UNITY_TARGETS
        FLAC sharpyuv hwy libzstd_static brotlienc libqpdf_object
        jxl_dec-obj jxl_enc-obj png_static archive_static wavpack
        libdeflate_static zlibstatic tag liblzma lz4_static libkanzi
        openjp2 flacout_lib
)

if(WIN32)
    list(APPEND NON_UNITY_TARGETS libgifsicle)
endif()

if(ENABLE_MATROSKA)
    list(APPEND NON_UNITY_TARGETS
            matroska2 corec ebml2 mkclean_lib file_test node_test
            ebmltree mkvtree mkcleanreg
    )
endif()

foreach(tgt IN LISTS NON_UNITY_TARGETS)
    if(TARGET ${tgt})
        set_target_properties(${tgt} PROPERTIES UNITY_BUILD OFF)
    endif()
endforeach()
