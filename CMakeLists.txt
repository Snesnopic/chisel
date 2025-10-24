
cmake_minimum_required(VERSION 3.20)

project(chisel VERSION 0.1 LANGUAGES C CXX)

if (CMAKE_VERSION VERSION_GREATER_EQUAL "3.30")
    set(CMAKE_POLICY_VERSION_MINIMUM "3.5")
endif()
if (WIN32 OR NOT APPLE)
    add_compile_definitions(HAVE_UINTPTR_T=1 HAVE_STDINT_H=1 HAVE_INTTYPES_H=1)
endif()
cmake_policy(SET CMP0069 NEW)
if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
    set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)
endif()
if (MSVC)
    add_compile_options(/std:c23)
endif()
set(CMAKE_C_STANDARD 23)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# ---------------- LTO detection and common flags (non-invasive) -------------
include(CheckCCompilerFlag)
include(CheckCXXCompilerFlag)

option(CHISEL_USE_THINLTO "Prefer -flto=thin when available" ON)

set(CHISEL_LTO_FLAG "")
if(CHISEL_USE_THINLTO)
    check_c_compiler_flag("-flto=thin" CHISEL_C_FLTO_THIN)
    check_cxx_compiler_flag("-flto=thin" CHISEL_CXX_FLTO_THIN)
    if(CHISEL_C_FLTO_THIN AND CHISEL_CXX_FLTO_THIN)
        set(CHISEL_LTO_FLAG "-flto=thin")
    endif()
endif()

if(CHISEL_LTO_FLAG STREQUAL "")
    check_c_compiler_flag("-flto" CHISEL_C_FLTO)
    check_cxx_compiler_flag("-flto" CHISEL_CXX_FLTO)
    if(CHISEL_C_FLTO AND CHISEL_CXX_FLTO)
        set(CHISEL_LTO_FLAG "-flto")
    endif()
endif()

# check for linker plugin flag (helps with LTO)
check_c_compiler_flag("-fuse-linker-plugin" CHISEL_C_FUSE_PLUGIN)
check_cxx_compiler_flag("-fuse-linker-plugin" CHISEL_CXX_FUSE_PLUGIN)
if(CHISEL_C_FUSE_PLUGIN AND CHISEL_CXX_FUSE_PLUGIN)
    set(CHISEL_FUSE_LINKER_PLUGIN_FLAG "-fuse-linker-plugin")
else()
    set(CHISEL_FUSE_LINKER_PLUGIN_FLAG "")
endif()

# --- DEFINE FLAGS AS LISTS (not a single quoted string) ---------------------
# use list form so add_compile_options expands to multiple args correctly
if(WIN32 AND NOT MSVC)
    add_compile_options(-msse2)
    set(CHISEL_COMMON_OPT_FLAGS -ffunction-sections -fdata-sections -fvisibility=hidden)
else()
    set(CHISEL_COMMON_OPT_FLAGS /Gy /Gw)
endif()
if(NOT WIN32)
    set(CHISEL_COMMON_OPT_FLAGS -ffunction-sections -fdata-sections -fvisibility=hidden)
endif()
# linker flags: platform-appropriate list
if(APPLE)
    # macOS linker: dead_strip removes unused code
    set(CHISEL_COMMON_LINK_FLAGS -Wl,-dead_strip)
elseif(WIN32)
    set(CHISEL_COMMON_LINK_FLAGS)
else()
    set(CHISEL_COMMON_LINK_FLAGS -Wl,--gc-sections -Wl,--as-needed)
endif()

# end lists -----------------------------------------------------------------
if(CHISEL_LTO_FLAG)
    message(STATUS "chisel: using LTO flag = ${CHISEL_LTO_FLAG}")
    # CHISEL_LINK_FLAGS is a list too
    set(CHISEL_LINK_FLAGS ${CHISEL_LTO_FLAG} ${CHISEL_FUSE_LINKER_PLUGIN_FLAG} ${CHISEL_COMMON_LINK_FLAGS})
else()
    message(STATUS "chisel: LTO not available on this compiler")
    set(CHISEL_LINK_FLAGS ${CHISEL_COMMON_LINK_FLAGS})
endif()

# CHISEL_PROPAGATE_* exclude -flto to avoid breaking subproject checks, keep lists
set(CHISEL_PROPAGATE_C_FLAGS ${CHISEL_COMMON_OPT_FLAGS})
set(CHISEL_PROPAGATE_CXX_FLAGS ${CHISEL_COMMON_OPT_FLAGS})
set(CHISEL_PROPAGATE_LINK_FLAGS ${CHISEL_COMMON_LINK_FLAGS})

# For cache variables we need a space-separated string: replace ';' with ' '
string(REPLACE ";" " " CHISEL_PROPAGATE_C_FLAGS_STR "${CHISEL_PROPAGATE_C_FLAGS}")
string(REPLACE ";" " " CHISEL_PROPAGATE_CXX_FLAGS_STR "${CHISEL_PROPAGATE_CXX_FLAGS}")
string(REPLACE ";" " " CHISEL_PROPAGATE_LINK_FLAGS_STR "${CHISEL_PROPAGATE_LINK_FLAGS}")

message(STATUS "chisel: propagate C flags (no-LTO) = ${CHISEL_PROPAGATE_C_FLAGS_STR}")
message(STATUS "chisel: propagate link flags (no-LTO) = ${CHISEL_PROPAGATE_LINK_FLAGS_STR}")

# Prepare environment variables for autotools builds (ExternalProject)
set(EXTERNAL_AUTOTOOLS_CFLAGS "${CHISEL_PROPAGATE_C_FLAGS_STR}")
set(EXTERNAL_AUTOTOOLS_CXXFLAGS "${CHISEL_PROPAGATE_CXX_FLAGS_STR}")
set(EXTERNAL_AUTOTOOLS_LDFLAGS "${CHISEL_PROPAGATE_LINK_FLAGS_STR}")
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
        if(APPLE)
            add_compile_options(-O3 -march=native -Wall -Wextra -Wpedantic ${CHISEL_COMMON_OPT_FLAGS})
        else()
            add_compile_options(-O3 -Wall -Wextra -Wpedantic ${CHISEL_COMMON_OPT_FLAGS})
        endif()
        if(CHISEL_LTO_FLAG)
            add_compile_options(${CHISEL_LTO_FLAG} ${CHISEL_FUSE_LINKER_PLUGIN_FLAG})
        endif()
        # CHISEL_LINK_FLAGS is a list
        add_link_options(${CHISEL_LINK_FLAGS})
    endif()
    if(NOT MINGW)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/release)

elseif(CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
    if (MSVC)
        add_compile_options(/O1 /GL /W4)
        add_link_options(/LTCG)
    else()
        add_compile_options(-Oz -Wall -Wextra -Wpedantic ${CHISEL_COMMON_OPT_FLAGS})
        if(CHISEL_LTO_FLAG)
            add_compile_options(${CHISEL_LTO_FLAG} ${CHISEL_FUSE_LINKER_PLUGIN_FLAG})
        endif()
        add_link_options(${CHISEL_LINK_FLAGS})
    endif()
    if(NOT MINGW)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/size)
endif()

include(FetchContent)
include(ExternalProject)

# Export propagated flags into CMake cache so FetchContent subprojects inherit them.
# NOTE: we intentionally do NOT include -flto in these propagated strings to avoid
# confusing subprojects' try_compile/try_run checks.
set(_new_c_flags "${CMAKE_C_FLAGS} ${CHISEL_PROPAGATE_C_FLAGS_STR}")
set(_new_cxx_flags "${CMAKE_CXX_FLAGS} ${CHISEL_PROPAGATE_CXX_FLAGS_STR}")
set(_new_exe_linker_flags "${CMAKE_EXE_LINKER_FLAGS} ${CHISEL_PROPAGATE_LINK_FLAGS_STR}")

if(NOT "${_new_c_flags}" STREQUAL "")
    set(CMAKE_C_FLAGS "${_new_c_flags}" CACHE STRING "Propagated C flags for external projects" FORCE)
endif()
if(NOT "${_new_cxx_flags}" STREQUAL "")
    set(CMAKE_CXX_FLAGS "${_new_cxx_flags}" CACHE STRING "Propagated CXX flags for external projects" FORCE)
endif()
if(NOT "${_new_exe_linker_flags}" STREQUAL "")
    set(CMAKE_EXE_LINKER_FLAGS "${_new_exe_linker_flags}" CACHE STRING "Propagated linker flags for external projects" FORCE)
endif()

# libmkclean
add_subdirectory("third_party/libmkclean")

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
set(CMAKE_SKIP_INSTALL_RULES ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/xz ${CMAKE_BINARY_DIR}/xz-build EXCLUDE_FROM_ALL)

target_include_directories(liblzma INTERFACE
        ${xz_SOURCE_DIR}/src/liblzma/api
)

set(LIBLZMA_INCLUDE_DIR "${xz_SOURCE_DIR}/src/liblzma/api" CACHE PATH "" FORCE)
set(LIBLZMA_LIBRARY "${xz_BINARY_DIR}/src/liblzma/liblzma.a" CACHE FILEPATH "" FORCE)
set(LZMA_STATIC_LIB "${xz_BINARY_DIR}/src/liblzma/liblzma.a" CACHE FILEPATH "liblzma static library")
set(LZMA_LIBRARY "${LZMA_STATIC_LIB}" CACHE STRING "lzma library" FORCE)
set(LZMA_LIBRARIES "${LZMA_STATIC_LIB}" CACHE STRING "lzma libraries" FORCE)
set(LZMA_FOUND TRUE CACHE BOOL "lzma found" FORCE)
set(LZMA_INCLUDE_DIR "${xz_SOURCE_DIR}/src/liblzma/api" CACHE PATH "lzma include" FORCE)

# ----- robust search for liblzma static artifact -----
if (EXISTS "${CMAKE_SOURCE_DIR}/third_party/xz")
    set(_lzma_search_roots
            "${CMAKE_BINARY_DIR}/third_party/xz-build"
            "${CMAKE_BINARY_DIR}/third_party/xz"
            "${CMAKE_SOURCE_DIR}/third_party/xz"
            "${CMAKE_SOURCE_DIR}/third_party/xz/src/liblzma"
            "${CMAKE_SOURCE_DIR}/third_party/xz/lib"
            "${CMAKE_BINARY_DIR}/xz-build/"
    )
    set(_found_lzma "")
    foreach(_root IN LISTS _lzma_search_roots)
        if (EXISTS "${_root}")
            file(GLOB_RECURSE _candidates
                    RELATIVE "${_root}"
                    "${_root}/**/liblzma*.a"
                    "${_root}/**/liblzma.a"
                    "${_root}/**/liblzma_static.a")
            foreach(_cand IN LISTS _candidates)
                set(_full "${_root}/${_cand}")
                if (EXISTS "${_full}")
                    set(_found_lzma "${_full}")
                    break()
                endif()
            endforeach()
        endif()
        if (_found_lzma)
            break()
        endif()
    endforeach()
    if(NOT _found_lzma)
        set(_found_lzma "${CMAKE_BINARY_DIR}/xz-build/liblzma.a")
    endif()
    if (_found_lzma)
        message(STATUS "found liblzma static: ${_found_lzma}")
        set(LZMA_STATIC_LIB "${_found_lzma}" CACHE FILEPATH "liblzma static library")
        set(LIBLZMA_STATIC_LIB "${_found_lzma}" CACHE FILEPATH "liblzma static library")
        if (EXISTS "${CMAKE_SOURCE_DIR}/third_party/xz/src/liblzma/api")
            set(LZMA_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/third_party/xz/src/liblzma/api" CACHE PATH "liblzma include")
            set(LIBLZMA_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/third_party/xz/src/liblzma/api" CACHE PATH "liblzma include")
        elseif (EXISTS "${CMAKE_SOURCE_DIR}/third_party/xz/src/liblzma")
            set(LZMA_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/third_party/xz/src/liblzma" CACHE PATH "liblzma include")
            set(LIBLZMA_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/third_party/xz/src/liblzma" CACHE PATH "liblzma include")
        endif()
        set(LZMA_LIBRARY "${LZMA_STATIC_LIB}" CACHE STRING "lzma library" FORCE)
        set(LIBLZMA_LIBRARY "${LZMA_STATIC_LIB}" CACHE STRING "lzma library" FORCE)
        set(LZMA_LIBRARIES "${LZMA_STATIC_LIB}" CACHE STRING "lzma libraries" FORCE)
        set(LIBLZMA_LIBRARIES "${LZMA_STATIC_LIB}" CACHE STRING "lzma libraries" FORCE)
        set(LZMA_FOUND TRUE CACHE BOOL "lzma found" FORCE)
        set(LIBLZMA_FOUND TRUE CACHE BOOL "lzma found" FORCE)
    else()
        message(WARNING "liblzma built but static lib not found; searched common locations. please locate liblzma.a and set LZMA_STATIC_LIB or adjust search paths.")
    endif()
endif()

# zstd
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED   OFF CACHE BOOL "" FORCE)
set(zstd_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/zstd/build/cmake")
set(zstd_BINARY_DIR "${CMAKE_BINARY_DIR}/third_party/zstd-build")
add_subdirectory(${zstd_SOURCE_DIR} ${zstd_BINARY_DIR} EXCLUDE_FROM_ALL)
set(ZSTD_FOUND TRUE CACHE BOOL "" FORCE)
set(ZSTD_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/third_party/zstd/lib" CACHE PATH "" FORCE)
set(ZSTD_INCLUDE_DIRS "${ZSTD_INCLUDE_DIR}" CACHE PATH "" FORCE)
set(ZSTD_LIBRARY libzstd_static CACHE STRING "" FORCE)
set(ZSTD_LIBRARIES libzstd_static CACHE STRING "" FORCE)

# libmagic
if(NOT WIN32)
    ExternalProject_Add(libmagic
            URL https://astron.com/pub/file/file-5.45.tar.gz
            URL_HASH SHA256=fc97f51029bb0e2c9f4e3bffefdaf678f0e039ee872b9de5c002a6d09c784d82
            CONFIGURE_COMMAND ./configure --disable-shared --enable-static --prefix=<INSTALL_DIR>
            BUILD_COMMAND make
            INSTALL_COMMAND make install
            BUILD_IN_SOURCE 1
            INSTALL_DIR ${CMAKE_BINARY_DIR}/libmagic-install
            BUILD_BYPRODUCTS
                ${CMAKE_BINARY_DIR}/libmagic-install/lib/libmagic.a
                ${CMAKE_BINARY_DIR}/libmagic-install/share/misc/magic.mgc
            CONFIGURE_ENV CFLAGS=${EXTERNAL_AUTOTOOLS_CFLAGS} CXXFLAGS=${EXTERNAL_AUTOTOOLS_CXXFLAGS} LDFLAGS=${EXTERNAL_AUTOTOOLS_LDFLAGS}
    )

    set(LIBMAGIC_INCLUDE_DIR ${CMAKE_BINARY_DIR}/libmagic-install/include)
    set(LIBMAGIC_LIB_DIR ${CMAKE_BINARY_DIR}/libmagic-install/lib)
    set(LIBMAGIC_LIB magic)

    # --- Embed compressed magic.mgc as header ---
    set(MAGIC_SRC "${CMAKE_BINARY_DIR}/libmagic-install/share/misc/magic.mgc")
    set(MAGIC_HEADER "${CMAKE_SOURCE_DIR}/libchisel/include/magic.mgc.h")
    set(MAGIC_COMPRESSED "${CMAKE_BINARY_DIR}/magic.mgc.gz")

    set(LIBMAGIC_INSTALL_DIR ${CMAKE_BINARY_DIR}/libmagic-install)
    file(MAKE_DIRECTORY ${LIBMAGIC_INSTALL_DIR}/include)

    add_library(LIBMAGIC STATIC IMPORTED GLOBAL)
    set_target_properties(LIBMAGIC PROPERTIES
            IMPORTED_LOCATION ${LIBMAGIC_INSTALL_DIR}/lib/libmagic.a
            INTERFACE_INCLUDE_DIRECTORIES ${LIBMAGIC_INSTALL_DIR}/include
    )

    add_dependencies(LIBMAGIC libmagic)
endif()

# zlib
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(zlib_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/zlib")
set(zlib_BINARY_DIR "${CMAKE_BINARY_DIR}/third_party/zlib-build")
set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

add_subdirectory(${zlib_SOURCE_DIR} third_party/zlib-build)

set(ZLIB_INCLUDE_DIR "${zlib_SOURCE_DIR};${zlib_BINARY_DIR}" CACHE PATH "zlib include dir")
set(ZLIB_INCLUDE_DIRS "${ZLIB_INCLUDE_DIR}" CACHE PATH "zlib include dirs")

if(MSVC)
    set(ZLIB_LIBRARY "${zlib_BINARY_DIR}/zlibstatic.lib" CACHE FILEPATH "zlib library")
else()
    set(ZLIB_LIBRARY "${zlib_BINARY_DIR}/libz.a" CACHE FILEPATH "zlib library")
endif()

set(ZLIB_LIBRARIES "${ZLIB_LIBRARY}" CACHE STRING "zlib libraries")
set(ZLIB_FOUND TRUE CACHE BOOL "zlib found")

set(ZLIB_H_PATH "${ZLIB_INCLUDE_DIR}" CACHE PATH "zlib include dir")
set(ZLIB_LIB_PATH "${ZLIB_LIBRARY}" CACHE FILEPATH "zlib library")

# libpng
set(PNG_SHARED OFF CACHE BOOL "" FORCE)
set(PNG_STATIC ON CACHE BOOL "" FORCE)
set(PNG_TESTS OFF CACHE BOOL "" FORCE)
set(libpng_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/libpng")
set(libpng_BINARY_DIR "${CMAKE_BINARY_DIR}/third_party/libpng-build")
add_subdirectory(${libpng_SOURCE_DIR} ${libpng_BINARY_DIR} EXCLUDE_FROM_ALL)

# libogg
set(libogg_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/libogg")
set(libogg_BINARY_DIR "${CMAKE_BINARY_DIR}/third_party/libogg-build")
add_subdirectory(${libogg_SOURCE_DIR} ${libogg_BINARY_DIR} EXCLUDE_FROM_ALL)

# libFLAC
set(BUILD_CXXLIBS OFF CACHE BOOL "" FORCE)
set(BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(INSTALL_MANPAGES OFF CACHE BOOL "" FORCE)
set(libflac_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/libflac")
set(libflac_BINARY_DIR "${CMAKE_BINARY_DIR}/third_party/libflac-build")
add_subdirectory(${libflac_SOURCE_DIR} ${libflac_BINARY_DIR} EXCLUDE_FROM_ALL)
if(TARGET man)
    set_target_properties(man PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()

# flexiGIF
set(FLEXIGIF_DIR ${CMAKE_SOURCE_DIR}/third_party/flexigif)
file(GLOB FLEXIGIF_SOURCES ${FLEXIGIF_DIR}/*.cpp)
add_library(flexigif STATIC ${FLEXIGIF_SOURCES})
target_include_directories(flexigif PUBLIC ${FLEXIGIF_DIR})
target_compile_features(flexigif PUBLIC cxx_std_11)

# wavpack
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE)
set(WAVPACK_BUILD_CLI OFF CACHE BOOL "Build command line programs" FORCE)
set(WAVPACK_BUILD_DOCS OFF CACHE BOOL "Build documentation" FORCE)
set(WAVPACK_BUILD_TESTS OFF CACHE BOOL "Build tests" FORCE)
set(wavpack_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/wavpack")
set(wavpack_BINARY_DIR "${CMAKE_BINARY_DIR}/third_party/wavpack-build")
add_subdirectory(${wavpack_SOURCE_DIR} ${wavpack_BINARY_DIR} EXCLUDE_FROM_ALL)

# libarchive
set(ENABLE_OPENSSL OFF CACHE BOOL "Disable OpenSSL in libarchive" FORCE)
set(ENABLE_TAR OFF CACHE BOOL "" FORCE)
set(ENABLE_CPIO OFF CACHE BOOL "" FORCE)
set(ENABLE_CAT OFF CACHE BOOL "" FORCE)
set(ENABLE_TEST OFF CACHE BOOL "" FORCE)
set(ENABLE_MBEDTLS OFF CACHE BOOL "" FORCE)
set(ENABLE_NETTLE  OFF CACHE BOOL "" FORCE)
set(ENABLE_LIBXML2 OFF CACHE BOOL "" FORCE)
set(ENABLE_EXPAT   OFF CACHE BOOL "" FORCE)
set(ENABLE_LZ4 OFF CACHE BOOL "" FORCE)
set(ENABLE_LIBB2 OFF CACHE BOOL "" FORCE)
set(ENABLE_PCREPOSIX  OFF CACHE BOOL "" FORCE)
set(ENABLE_PCRE2POSIX OFF CACHE BOOL "" FORCE)
set(ENABLE_SHARED OFF CACHE BOOL "" FORCE)
set(ENABLE_STATIC ON CACHE BOOL "" FORCE)
set(libarchive_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/libarchive")
set(libarchive_BINARY_DIR "${CMAKE_BINARY_DIR}/third_party/libarchive-build")
add_subdirectory(${libarchive_SOURCE_DIR} ${libarchive_BINARY_DIR} EXCLUDE_FROM_ALL)

# --- mozjpeg via ExternalProject ---
include(ExternalProject)
if(MSVC)
    set(_mozjpeg_lib "${CMAKE_BINARY_DIR}/mozjpeg/src/mozjpeg-build/jpeg-static.lib")
elseif(MINGW)
    set(_mozjpeg_lib "${CMAKE_BINARY_DIR}/mozjpeg/src/mozjpeg-build/libjpeg.a")
else()
    set(_mozjpeg_lib "${CMAKE_BINARY_DIR}/mozjpeg/src/mozjpeg-build/libjpeg.a")
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

# libEBML
set(libebml_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/libebml")
set(libebml_BINARY_DIR "${CMAKE_BINARY_DIR}/third_party/libebml-build")
add_subdirectory(${libebml_SOURCE_DIR} ${libebml_BINARY_DIR} EXCLUDE_FROM_ALL)
target_include_directories(ebml PUBLIC
        ${libebml_BINARY_DIR}
)
list(APPEND CMAKE_PREFIX_PATH "${CMAKE_SOURCE_DIR}/cmake")

# libMatroska
set(libmatroska_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/libmatroska")
set(libmatroska_BINARY_DIR "${CMAKE_BINARY_DIR}/third_party/libmatroska-build")
add_subdirectory(${libmatroska_SOURCE_DIR} ${libmatroska_BINARY_DIR} EXCLUDE_FROM_ALL)

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
add_subdirectory(${libwebp_SOURCE_DIR} ${libwebp_BINARY_DIR} EXCLUDE_FROM_ALL)
add_library(thirdparty_webp ALIAS webp)
add_library(thirdparty_webpdecoder ALIAS webpdecoder)
add_library(thirdparty_webpdemux ALIAS webpdemux)
add_library(thirdparty_webpmux ALIAS libwebpmux)

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
add_subdirectory(${qpdf_SOURCE_DIR} ${qpdf_BINARY_DIR} EXCLUDE_FROM_ALL)

# zopfli
set(zopfli_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/zopfli")
set(zopfli_BINARY_DIR "${CMAKE_BINARY_DIR}/third_party/zopfli-build")
add_subdirectory(${zopfli_SOURCE_DIR} ${zopfli_BINARY_DIR} EXCLUDE_FROM_ALL)

# libjxl
set(JPEGXL_ENABLE_DOXYGEN OFF CACHE BOOL "Turned off JXL doxygen" FORCE)
set(JPEGXL_ENABLE_TOOLS OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_DEVTOOLS OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_PLUGINS OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_MANPAGES OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_BENCHMARK OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(libjxl_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/libjxl")
set(libjxl_BINARY_DIR "${CMAKE_BINARY_DIR}/third_party/libjxl-build")
add_subdirectory(${libjxl_SOURCE_DIR} ${libjxl_BINARY_DIR} EXCLUDE_FROM_ALL)

# libtiff
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(tiff-tools OFF CACHE BOOL "" FORCE)
set(tiff-tests OFF CACHE BOOL "" FORCE)
set(tiff-docs OFF CACHE BOOL "" FORCE)
set(libtiff_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/libtiff")
set(libtiff_BINARY_DIR "${CMAKE_BINARY_DIR}/third_party/libtiff-build")
add_subdirectory(${libtiff_SOURCE_DIR} ${libtiff_BINARY_DIR} EXCLUDE_FROM_ALL)

# sqlite3
set(sqlite3_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/sqlite3")
set(sqlite3_BINARY_DIR "${CMAKE_BINARY_DIR}/third_party/sqlite3-build")
add_subdirectory(${sqlite3_SOURCE_DIR} ${sqlite3_BINARY_DIR} EXCLUDE_FROM_ALL)

# libmseed
set(libmseed_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/libmseed")
if(WIN32)
    add_library(libmseed STATIC
            ${libmseed_SOURCE_DIR}/fileutils.c
            ${libmseed_SOURCE_DIR}/genutils.c
            ${libmseed_SOURCE_DIR}/msio.c
            ${libmseed_SOURCE_DIR}/lookup.c
            ${libmseed_SOURCE_DIR}/yyjson.c
            ${libmseed_SOURCE_DIR}/msrutils.c
            ${libmseed_SOURCE_DIR}/extraheaders.c
            ${libmseed_SOURCE_DIR}/pack.c
            ${libmseed_SOURCE_DIR}/packdata.c
            ${libmseed_SOURCE_DIR}/tracelist.c
            ${libmseed_SOURCE_DIR}/gmtime64.c
            ${libmseed_SOURCE_DIR}/crc32c.c
            ${libmseed_SOURCE_DIR}/parseutils.c
            ${libmseed_SOURCE_DIR}/unpack.c
            ${libmseed_SOURCE_DIR}/unpackdata.c
            ${libmseed_SOURCE_DIR}/selection.c
            ${libmseed_SOURCE_DIR}/logging.c
    )
    target_include_directories(libmseed PUBLIC ${libmseed_SOURCE_DIR})
else()
    set(libmseed_BINARY_DIR "${CMAKE_BINARY_DIR}/libmseed-build")
    set(libmseed_INSTALL_DIR "${CMAKE_BINARY_DIR}/libmseed-install")
    ExternalProject_Add(libmseed
            SOURCE_DIR ${libmseed_SOURCE_DIR}
            CONFIGURE_COMMAND ""
            BUILD_COMMAND make
            INSTALL_COMMAND make install PREFIX=${libmseed_INSTALL_DIR}
            BUILD_IN_SOURCE 1
            BUILD_BYPRODUCTS ${libmseed_SOURCE_DIR}/libmseed.a
    )
endif()

# gifsicle
if(NOT WIN32)
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
            # third_party/gifsicle/src/strerror.c
            third_party/gifsicle/src/support.c
            third_party/gifsicle/src/ungifwrt.c
    )
    if(WIN32 OR NOT APPLE)
        target_compile_definitions(giflib PRIVATE HAVE_UINTPTR_T=1)
    endif()
    target_include_directories(giflib PUBLIC
            third_party/gifsicle/include
            third_party/gifsicle/src
    )
endif()

# Monkey's Audio
set(MACLIB_HEADER "${CMAKE_SOURCE_DIR}/third_party/MACLib/Shared/MACLib.h")
if(EXISTS "${MACLIB_HEADER}")
    file(READ "${MACLIB_HEADER}" MACLIB_CONTENTS)
    string(FIND "${MACLIB_CONTENTS}" "#include \"All.h\"" _found)
    if(_found EQUAL -1)
        message(STATUS "Patching MACLib.h to include All.h")
        file(WRITE "${MACLIB_HEADER}.patched${CMAKE_BUILD_TYPE}" "#include \"All.h\"\n${MACLIB_CONTENTS}")
        file(RENAME "${MACLIB_HEADER}.patched${CMAKE_BUILD_TYPE}" "${MACLIB_HEADER}")
    else()
        message(STATUS "MACLib.h already includes All.h")
    endif()
endif()
set(BUILD_UTIL FALSE CACHE BOOL "" FORCE)
set(BUILD_SHARED FALSE CACHE BOOL "" FORCE)
add_subdirectory(third_party/MACLib EXCLUDE_FROM_ALL)
target_include_directories(MAC PUBLIC third_party/MACLib/Shared)
if(WIN32)
    target_compile_definitions(MAC PRIVATE NO_MFC)
    set(MACLIB_STDAFX "${CMAKE_SOURCE_DIR}/third_party/MACLib/Source/MacDll/stdafx.h")
    if(EXISTS "${MACLIB_STDAFX}")
        file(READ "${MACLIB_STDAFX}" _contents)
        string(REPLACE "#include <afxwin.h>"  "//#include <afxwin.h>"  _contents "${_contents}")
        string(REPLACE "#include <afxext.h>"  "//#include <afxext.h>"  _contents "${_contents}")
        string(REPLACE "#include \"MFCGlobals.h\"" "//#include \"MFCGlobals.h\"" _contents "${_contents}")
        file(WRITE "${MACLIB_STDAFX}" "${_contents}")
    endif()
endif()

# zopfli include hint for targets (safe to keep)
target_include_directories(zopflipng PUBLIC
        ${zopfli_SOURCE_DIR}/src/zopflipng
        ${zopfli_SOURCE_DIR}/src/zopfli
)

# common libraries for all platforms (exposed to libchisel/chisel_cli)
set(CHISEL_LIBS
        FLAC
        MAC
        png_static
        archive_static
        ogg
        ebml
        matroska
        wavpack
        flexigif
        thirdparty_webp
        thirdparty_webpdecoder
        thirdparty_webpdemux
        thirdparty_webpmux
        libqpdf
        libzopfli
        libzopflipng
        jxl
        jxl_threads
        tiff
        SQLite::SQLite3
        mkclean_lib
        zlibstatic
)
if(NOT WIN32)
    list(APPEND CHISEL_LIBS ${libmseed_SOURCE_DIR}/libmseed.a giflib)
endif()

if(UNIX)
    list(APPEND CHISEL_LIBS
            LIBMAGIC
    )
elseif(WIN32)
    list(APPEND CHISEL_LIBS
            libmseed
    )
endif()

add_subdirectory(libchisel)
add_dependencies(libchisel copy_mozjpeg_jconfig)
add_subdirectory(chisel_cli)
