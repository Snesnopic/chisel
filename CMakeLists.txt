cmake_minimum_required(VERSION 3.20)

project(chisel VERSION 1.2.0 LANGUAGES C CXX)

option(CHISEL_BUILD_CLI "Build the chisel command-line executable" ON)
option(ENABLE_OPTIVORBIS "Enable rust optivorbis bridge" ON)
option(ENABLE_MP3PACKER "Enable OCaml mp3packer integration" ON)
option(ENABLE_MATROSKA "Enable LibEBML and LibMatroska integration" ON)

set(CMAKE_UNITY_BUILD TRUE)

set(CMAKE_INTERPROCEDURAL_OPTIMIZATION OFF)
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

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

include(CheckCCompilerFlag)
include(CheckCXXCompilerFlag)

if(APPLE)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        add_link_options("-static-libgcc")
        add_link_options("-static-libstdc++")
    endif()
endif()

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
    else()
        add_compile_options(-O3 -w)
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

    add_library(bz2_static STATIC
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
    add_subdirectory("third_party/libmkclean")
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
add_subdirectory(third_party/xz ${CMAKE_BINARY_DIR}/xz-build EXCLUDE_FROM_ALL)
set(LIBLZMA_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/third_party/xz/src/liblzma/api" CACHE PATH "liblzma include" FORCE)

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

# libmagic
if(NOT WIN32)
    # fetch libmagic source
    FetchContent_Declare(
            libmagic_ext
            URL https://astron.com/pub/file/file-5.45.tar.gz
            URL_HASH SHA256=fc97f51029bb0e2c9f4e3bffefdaf678f0e039ee872b9de5c002a6d09c784d82
    )
    FetchContent_MakeAvailable(libmagic_ext)

    set(MAGIC_SRC "${libmagic_ext_SOURCE_DIR}/src")
    set(MAGIC_BIN "${libmagic_ext_BINARY_DIR}/src")
    file(MAKE_DIRECTORY "${MAGIC_BIN}")

    set(LIBMAGIC_SOURCES
            "${MAGIC_SRC}/apprentice.c"
            "${MAGIC_SRC}/apptype.c"
            "${MAGIC_SRC}/ascmagic.c"
            "${MAGIC_SRC}/buffer.c"
            "${MAGIC_SRC}/cdf.c"
            "${MAGIC_SRC}/cdf_time.c"
            "${MAGIC_SRC}/compress.c"
            "${MAGIC_SRC}/der.c"
            "${MAGIC_SRC}/encoding.c"
            "${MAGIC_SRC}/fsmagic.c"
            "${MAGIC_SRC}/funcs.c"
            "${MAGIC_SRC}/is_csv.c"
            "${MAGIC_SRC}/is_json.c"
            "${MAGIC_SRC}/is_tar.c"
            "${MAGIC_SRC}/is_simh.c"
            "${MAGIC_SRC}/magic.c"
            "${MAGIC_SRC}/print.c"
            "${MAGIC_SRC}/readcdf.c"
            "${MAGIC_SRC}/readelf.c"
            "${MAGIC_SRC}/softmagic.c"
    )

    if(APPLE)
        set(MAGIC_CONFIG_H "
#define HAVE_STDINT_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_UNISTD_H 1
#define HAVE_FCNTL_H 1
#define HAVE_MMAP 1
#define HAVE_STRNDUP 1
#define HAVE_ASPRINTF 1
#define HAVE_VASPRINTF 1
#define HAVE_STRLCPY 1
#define HAVE_STRLCAT 1
#define HAVE_GETLINE 1
#define HAVE_FMTCHECK 1
#define VERSION \"5.45\"
        ")
    else()
        list(APPEND LIBMAGIC_SOURCES
                "${MAGIC_SRC}/fmtcheck.c"
                "${MAGIC_SRC}/strlcpy.c"
                "${MAGIC_SRC}/strlcat.c"
        )
        set(MAGIC_CONFIG_H "
#define HAVE_STDINT_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_UNISTD_H 1
#define HAVE_FCNTL_H 1
#define HAVE_MMAP 1
#define HAVE_STRNDUP 1
#define HAVE_ASPRINTF 1
#define HAVE_VASPRINTF 1
#define HAVE_GETLINE 1
#undef HAVE_STRLCPY
#undef HAVE_STRLCAT
#undef HAVE_FMTCHECK
#define VERSION \"5.45\"
        ")
    endif()

    if(NOT EXISTS "${MAGIC_BIN}/magic.h")
        file(READ "${MAGIC_SRC}/magic.h.in" magic_h_in)
        string(REPLACE "X.YY" "5.45" magic_h_out "${magic_h_in}")
        file(WRITE "${MAGIC_BIN}/magic.h" "${magic_h_out}")
    endif()

    file(WRITE "${MAGIC_BIN}/config.h" "${MAGIC_CONFIG_H}")

    add_library(LIBMAGIC STATIC ${LIBMAGIC_SOURCES})

    target_include_directories(LIBMAGIC PUBLIC
            $<BUILD_INTERFACE:${MAGIC_BIN}>
            $<BUILD_INTERFACE:${MAGIC_SRC}>
    )
    target_compile_definitions(LIBMAGIC PRIVATE HAVE_CONFIG_H)

    set(MAGIC_HEADER "${CMAKE_SOURCE_DIR}/libchisel/include/magic.mgc.h")
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
    set(ZLIB_LIBRARY "${zlib_BINARY_DIR}/Release/zlibstatic.lib" CACHE FILEPATH "zlib library")
else()
    set(ZLIB_LIBRARY "${zlib_BINARY_DIR}/libz.a" CACHE FILEPATH "zlib library")
endif()

set(ZLIB_LIBRARIES "${ZLIB_LIBRARY}" CACHE STRING "zlib libraries")
set(ZLIB_FOUND TRUE CACHE BOOL "zlib found")

set(ZLIB_H_PATH "${ZLIB_INCLUDE_DIR}" CACHE PATH "zlib include dir")
set(ZLIB_LIB_PATH "${ZLIB_LIBRARY}" CACHE FILEPATH "zlib library")

set(ENABLE_STATIC_RUNTIME OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_BINDINGS OFF CACHE BOOL "" FORCE)
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
set(libpng_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/libpng")
set(libpng_BINARY_DIR "${CMAKE_BINARY_DIR}/third_party/libpng-build")
add_subdirectory(${libpng_SOURCE_DIR} ${libpng_BINARY_DIR} EXCLUDE_FROM_ALL)

# pugixml
add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/pugixml EXCLUDE_FROM_ALL)

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
list(REMOVE_ITEM FLEXIGIF_SOURCES "${FLEXIGIF_DIR}/flexiGIF.cpp")
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
set(ENABLE_LZ4 OFF CACHE BOOL "" FORCE)
set(ENABLE_BZip2 ON CACHE BOOL "" FORCE)
set(ENABLE_LIBB2 ON CACHE BOOL "" FORCE)
set(ENABLE_PCREPOSIX  OFF CACHE BOOL "" FORCE)
set(ENABLE_PCRE2POSIX OFF CACHE BOOL "" FORCE)
set(ENABLE_SHARED OFF CACHE BOOL "" FORCE)
set(ENABLE_STATIC ON CACHE BOOL "" FORCE)
set(libarchive_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/libarchive")
set(libarchive_BINARY_DIR "${CMAKE_BINARY_DIR}/third_party/libarchive-build")
# set(HAVE_LIBLZMA 1 CACHE INTERNAL "Force liblzma found")
# set(HAVE_LZMA_H 1 CACHE INTERNAL "Force lzma.h found")
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
set(BMPLIB_DIR "${CMAKE_SOURCE_DIR}/third_party/bmplib")
set(BMPLIB_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/bmplib")

file(MAKE_DIRECTORY ${BMPLIB_BUILD_DIR})

if(NOT EXISTS "${BMPLIB_DIR}/config.h")
    file(READ "${BMPLIB_DIR}/config.h.in" BMP_CONFIG_CONTENT)
    string(REPLACE "@libversion@" "1.8.0" BMP_CONFIG_CONTENT "${BMP_CONFIG_CONTENT}")
    string(REPLACE "@insanity_limit_mb@" "1024" BMP_CONFIG_CONTENT "${BMP_CONFIG_CONTENT}")
    file(WRITE "${BMPLIB_DIR}/config.h" "${BMP_CONFIG_CONTENT}")
endif()

set(BMP_HEADER_ORIGINAL "${BMPLIB_DIR}/bmplib.h")
set(BMP_HEADER_PATCHED  "${BMPLIB_BUILD_DIR}/bmplib.h")

if(WIN32)
    file(READ "${BMP_HEADER_ORIGINAL}" BMP_HEADER_CONTENT)
    string(REPLACE
            "#if defined (WIN32) || defined (_WIN32)"
            "#if 0"
            BMP_HEADER_CONTENT "${BMP_HEADER_CONTENT}")
    file(WRITE "${BMP_HEADER_ORIGINAL}" "${BMP_HEADER_CONTENT}")
endif()

set(HUFFMAN_CODES_H "${BMPLIB_BUILD_DIR}/huffman-codes.h")
if(CMAKE_CROSSCOMPILING)
    add_custom_command(
            OUTPUT "${BMPLIB_BUILD_DIR}/gen-huffman-host"
            COMMAND env -u CFLAGS -u CXXFLAGS -u LDFLAGS cc "${BMPLIB_DIR}/gen-huffman.c" -I"${BMPLIB_DIR}" -include stdbool.h -o "${BMPLIB_BUILD_DIR}/gen-huffman-host"
            DEPENDS "${BMPLIB_DIR}/gen-huffman.c"
            COMMENT "Compiling host gen-huffman for cross-compilation"
    )

    add_custom_command(
            OUTPUT "${HUFFMAN_CODES_H}"
            COMMAND "${BMPLIB_BUILD_DIR}/gen-huffman-host" "${HUFFMAN_CODES_H}"
            DEPENDS "${BMPLIB_BUILD_DIR}/gen-huffman-host"
            COMMENT "Generating huffman-codes.h for bmplib via host tool"
    )
else()
    add_executable(gen-huffman "${BMPLIB_DIR}/gen-huffman.c")
    target_include_directories(gen-huffman PRIVATE "${BMPLIB_DIR}")

    if(MSVC)
        target_compile_options(gen-huffman PRIVATE "/FIstdbool.h")
    else()
        target_compile_options(gen-huffman PRIVATE "-include" "stdbool.h")
    endif()

    set_target_properties(gen-huffman PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${BMPLIB_BUILD_DIR}"
            RUNTIME_OUTPUT_DIRECTORY_DEBUG "${BMPLIB_BUILD_DIR}"
            RUNTIME_OUTPUT_DIRECTORY_RELEASE "${BMPLIB_BUILD_DIR}"
    )

    add_custom_command(
            OUTPUT "${HUFFMAN_CODES_H}"
            COMMAND $<TARGET_FILE:gen-huffman> "${HUFFMAN_CODES_H}"
            DEPENDS gen-huffman
            COMMENT "Generating huffman-codes.h for bmplib"
    )
endif()

add_custom_target(generate_bmplib_headers DEPENDS "${HUFFMAN_CODES_H}")

file(GLOB BMPLIB_SOURCES "${BMPLIB_DIR}/*.c")
list(REMOVE_ITEM BMPLIB_SOURCES
        "${BMPLIB_DIR}/gen-huffman.c"
        "${BMPLIB_DIR}/test-fileio-pipes.c"
        "${BMPLIB_DIR}/test-read-conversions.c"
        "${BMPLIB_DIR}/test-read-io.c"
        "${BMPLIB_DIR}/test-write-io.c"
)

if(MSVC)
    list(FILTER BMPLIB_SOURCES EXCLUDE REGEX "bmp-read-loadimage\\.c$")

    set(BMP_ORIGINAL "${BMPLIB_DIR}/bmp-read-loadimage.c")
    set(BMP_PATCHED  "${BMPLIB_BUILD_DIR}/bmp-read-loadimage.c")

    file(READ "${BMP_ORIGINAL}" BMP_SRC_CONTENT)
    string(REPLACE "[static " "[" BMP_SRC_CONTENT "${BMP_SRC_CONTENT}")
    file(WRITE "${BMP_PATCHED}" "${BMP_SRC_CONTENT}")

    list(APPEND BMPLIB_SOURCES "${BMP_PATCHED}")
endif()

# --- mozjpeg via ExternalProject ---
include(ExternalProject)
if(MSVC)
    set(_mozjpeg_lib "${CMAKE_BINARY_DIR}/mozjpeg/src/mozjpeg-build/Release/jpeg-static.lib")
elseif(MINGW)
    set(_mozjpeg_lib "${CMAKE_BINARY_DIR}/mozjpeg/src/mozjpeg-build/libjpeg.a")
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
        ${MOZJPEG_CROSS_ARGS}
        -DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}
        -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
        -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
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
if(ENABLE_MATROSKA)
    set(libebml_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/libebml")
    set(libebml_BINARY_DIR "${CMAKE_BINARY_DIR}/third_party/libebml-build")
    add_subdirectory(${libebml_SOURCE_DIR} ${libebml_BINARY_DIR} EXCLUDE_FROM_ALL)
    list(APPEND CMAKE_PREFIX_PATH "${CMAKE_SOURCE_DIR}/cmake")
    list(APPEND libebml_SOURCE_DIR
            "${CMAKE_SOURCE_DIR}/third_party/libebml/ebml"
            "${CMAKE_BINARY_DIR}/third_party/libebml-build"
    )
# libMatroska
    set(libmatroska_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/libmatroska")
    set(libmatroska_BINARY_DIR "${CMAKE_BINARY_DIR}/third_party/libmatroska-build")
    add_subdirectory(${libmatroska_SOURCE_DIR} ${libmatroska_BINARY_DIR} EXCLUDE_FROM_ALL)
    list(APPEND libmatroska_SOURCE_DIR
            "${CMAKE_BINARY_DIR}/third_party/libmatroska-build"
    )
    target_include_directories(matroska PRIVATE
            ${libebml_BINARY_DIR}
    )
endif()
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
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LIBMSEED_URL OFF CACHE BOOL "" FORCE)

add_subdirectory(third_party/libmseed EXCLUDE_FROM_ALL)

# gifsicle
set(BUILD_GIFSICLE_CLI OFF CACHE BOOL "" FORCE)
set(BUILD_GIFDIFF OFF CACHE BOOL "" FORCE)
set(BUILD_GIFVIEW OFF CACHE BOOL "" FORCE)

add_subdirectory(third_party/gifsicle)

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

# mp3packer (ocaml)
if(ENABLE_MP3PACKER)
    include(cmake/mp3packer.cmake)
    add_mp3packer_library(mp3packer_static "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mp3packer")

    add_subdirectory(third_party/vbrfix)
endif()

# common libraries for all platforms (exposed to libchisel/chisel_cli)
set(CHISEL_LIBS
        FLAC
        MAC
        png_static
        archive_static
        ogg
        mozjpeg_static
        wavpack
        flexigif
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
        SQLite3
        zlibstatic
        tag
        libgifsicle
        pugixml
        mseed_static
)

if(ENABLE_MATROSKA)
    list(APPEND CHISEL_LIBS ebml matroska mkclean_lib)
    add_compile_definitions(HAVE_MATROSKA=1)
endif()

if(ENABLE_MP3PACKER)
    list(APPEND CHISEL_LIBS mp3packer_static)
    list(APPEND CHISEL_LIBS vbrfix_lib)
    add_compile_definitions(HAVE_MP3PACKER=1)
endif()

if(NOT WIN32)
    list(APPEND CHISEL_LIBS
            LIBMAGIC
    )
endif()
if(ENABLE_OPTIVORBIS)
    add_subdirectory(third_party/corrosion)
    add_compile_definitions(HAVE_OPTIVORBIS=1)
endif()

add_subdirectory(libchisel)
add_dependencies(libchisel copy_mozjpeg_jconfig)

target_sources(libchisel PRIVATE ${BMPLIB_SOURCES})

target_include_directories(libchisel SYSTEM PRIVATE
        "${BMPLIB_DIR}"
        "${BMPLIB_BUILD_DIR}"
)

target_include_directories(libchisel SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/third_party/gifsicle/include"
        "${CMAKE_SOURCE_DIR}/third_party/gifsicle/src"
)
add_dependencies(libchisel generate_bmplib_headers)

if(CHISEL_BUILD_CLI)
    add_subdirectory(chisel_cli)
endif()

# we need to edit bmplib and libmkclean before we can do this
set_target_properties(libchisel PROPERTIES UNITY_BUILD OFF)
if(CHISEL_BUILD_CLI)
    set_target_properties(chisel PROPERTIES UNITY_BUILD OFF)
endif()
set_target_properties(FLAC PROPERTIES UNITY_BUILD OFF)
set_target_properties(sharpyuv PROPERTIES UNITY_BUILD OFF)
set_target_properties(hwy PROPERTIES UNITY_BUILD OFF)
set_target_properties(libzstd_static PROPERTIES UNITY_BUILD OFF)
set_target_properties(brotlienc PROPERTIES UNITY_BUILD OFF)
set_target_properties(LIBMAGIC PROPERTIES UNITY_BUILD OFF)
set_target_properties(libqpdf_object PROPERTIES UNITY_BUILD OFF)
set_target_properties(jxl_dec-obj PROPERTIES UNITY_BUILD OFF)
set_target_properties(jxl_enc-obj PROPERTIES UNITY_BUILD OFF)
set_target_properties(png_static PROPERTIES UNITY_BUILD OFF)
set_target_properties(archive_static PROPERTIES UNITY_BUILD OFF)
set_target_properties(wavpack PROPERTIES UNITY_BUILD OFF)
set_target_properties(libdeflate_static PROPERTIES UNITY_BUILD OFF)
set_target_properties(zlibstatic PROPERTIES UNITY_BUILD OFF)
set_target_properties(tag PROPERTIES UNITY_BUILD OFF)

if(ENABLE_MATROSKA)
    set_target_properties(matroska2 PROPERTIES UNITY_BUILD OFF)
    set_target_properties(corec PROPERTIES UNITY_BUILD OFF)
    set_target_properties(ebml2 PROPERTIES UNITY_BUILD OFF)
    set_target_properties(mkclean_lib PROPERTIES UNITY_BUILD OFF)
    set_target_properties("file_test" PROPERTIES UNITY_BUILD OFF)
    set_target_properties("node_test" PROPERTIES UNITY_BUILD OFF)
    set_target_properties("ebmltree" PROPERTIES UNITY_BUILD OFF)
    set_target_properties("mkvtree" PROPERTIES UNITY_BUILD OFF)
endif()

set_target_properties(liblzma PROPERTIES UNITY_BUILD OFF)
set_target_properties(libdeflate_static PROPERTIES UNITY_BUILD OFF)
set_target_properties(tag PROPERTIES UNITY_BUILD OFF)
