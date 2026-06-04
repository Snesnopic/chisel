set(LIBCHISEL_SOURCES
        include/ape_processor.hpp
        include/archive_processor.hpp
        include/event_bus.hpp
        include/events.hpp
        include/file_type.hpp
        include/flac_processor.hpp
        include/flexigif_processor.hpp
        include/gif_processor.hpp
        include/jpeg_processor.hpp
        include/pe_processor.hpp
        include/jxl_processor.hpp
        include/kanzi_processor.hpp
        include/vcf_processor.hpp
        include/json_processor.hpp
        include/jp2_processor.hpp
        include/log_sink.hpp
        include/logger.hpp
        include/mime_detector.hpp
        include/mkv_processor.hpp
        include/mseed_processor.hpp
        include/odf_processor.hpp
        include/ooxml_processor.hpp
        include/pdf_processor.hpp
        include/png_processor.hpp
        include/processor.hpp
        include/processor_executor.hpp
        include/processor_registry.hpp
        include/random_utils.hpp
        include/sqlite_processor.hpp
        include/thread_pool.hpp
        include/tiff_processor.hpp
        include/wavpack_processor.hpp
        include/webp_processor.hpp
        include/zopflipng_processor.hpp
        src/processors/ape_processor.cpp
        src/processors/archive_processor.cpp
        src/processors/flac_processor.cpp
        src/processors/flexigif_processor.cpp
        src/processors/gif_processor.cpp
        src/processors/jpeg_processor.cpp
        src/processors/jxl_processor.cpp
        src/processors/kanzi_processor.cpp
        src/processors/mkv_processor.cpp
        src/processors/mseed_processor.cpp
        src/processors/odf_processor.cpp
        src/processors/ooxml_processor.cpp
        src/processors/pdf_processor.cpp
        src/processors/pe_processor.cpp
        src/processors/png_processor.cpp
        src/processors/sqlite_processor.cpp
        src/processors/tiff_processor.cpp
        src/processors/vcf_processor.cpp
        src/processors/json_processor.cpp
        src/processors/jp2_processor.cpp
        src/processors/wavpack_processor.cpp
        src/processors/webp_processor.cpp
        src/processors/zopflipng_processor.cpp
        src/utils/logger.cpp
        src/utils/mime_detector.cpp
        src/utils/processor_executor.cpp
        src/utils/processor_registry.cpp
        src/utils/random_utils.cpp
        src/utils/thread_pool.cpp
        include/tga_processor.hpp
        src/processors/tga_processor.cpp
        include/file_utils.hpp
        src/utils/file_utils.cpp
        include/audio_metadata_util.hpp
        src/utils/audio_metadata_util.cpp
        include/mpeg_processor.hpp
        src/processors/mpeg_processor.cpp
        include/wav_processor.hpp
        src/processors/wav_processor.cpp
        include/mp4_processor.hpp
        include/ogg_processor.hpp
        src/processors/mp4_processor.cpp
        src/processors/ogg_processor.cpp
        include/aiff_processor.hpp
        src/processors/aiff_processor.cpp
        include/bmp_processor.hpp
        src/processors/bmp_processor.cpp
        include/pnm_processor.hpp
        src/processors/pnm_processor.cpp
        include/chisel.hpp
        src/utils/chisel.cpp
        include/xml_processor.hpp
        src/processors/xml_processor.cpp
        include/options.hpp
        include/zopfli_compressor.hpp
        src/utils/zopfli_compressor.cpp
        include/woff2_processor.hpp
        src/processors/woff2_processor.cpp
        include/brotli_processor.hpp
        src/processors/brotli_processor.cpp
        include/asf_processor.hpp
        src/processors/asf_processor.cpp
        include/dsdiff_processor.hpp
        src/processors/dsdiff_processor.cpp
        include/dsf_processor.hpp
        src/processors/dsf_processor.cpp
        include/mpc_processor.hpp
        src/processors/mpc_processor.cpp
        include/tta_processor.hpp
        src/processors/tta_processor.cpp
        include/cfbf_processor.hpp
        src/processors/cfbf_processor.cpp
        include/lzma_processor.hpp
        src/processors/lzma_processor.cpp
        include/zstd_processor.hpp
        src/processors/zstd_processor.cpp
        include/bzip2_processor.hpp
        src/processors/bzip2_processor.cpp
        include/ico_processor.hpp
        src/processors/ico_processor.cpp
        include/swf_processor.hpp
        src/processors/swf_processor.cpp
        include/gft_processor.hpp
        src/processors/gft_processor.cpp
        include/rdb_processor.hpp
        src/processors/rdb_processor.cpp
        include/icns_processor.hpp
        src/processors/icns_processor.cpp
        include/woff_processor.hpp
        src/processors/woff_processor.cpp

        ${CMAKE_SOURCE_DIR}/third_party/kanzi-cpp/src/app/BlockCompressor.cpp
        ${CMAKE_SOURCE_DIR}/third_party/kanzi-cpp/src/app/BlockDecompressor.cpp
        ${CMAKE_SOURCE_DIR}/third_party/kanzi-cpp/src/app/InfoPrinter.cpp
)
if(ENABLE_OPTIVORBIS)
    corrosion_import_crate(MANIFEST_PATH "rust_bridge/Cargo.toml")
endif()

add_library(libchisel STATIC ${LIBCHISEL_SOURCES})

target_compile_definitions(libchisel PRIVATE
        CHISEL_VERSION="${PROJECT_VERSION}"
        CONCURRENCY_DISABLED
)

target_include_directories(libchisel
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        PRIVATE
        src
        SYSTEM PRIVATE
        ${libflac_SOURCE_DIR}/include
        ${libflac_BINARY_DIR}/include
        ${libpng_SOURCE_DIR}
        ${libpng_BINARY_DIR}
        ${JPEG_INCLUDE_DIR}
        ${libarchive_SOURCE_DIR}/libarchive
        ${libarchive_BINARY_DIR}/libarchive
        ${libogg_SOURCE_DIR}/include
        ${libogg_BINARY_DIR}/include
        ${qpdf_SOURCE_DIR}/include
        ${qpdf_BINARY_DIR}/include
        ${zopfli_SOURCE_DIR}/include
        ${zopfli_BINARY_DIR}/include
        ${zopfli_SOURCE_DIR}/src/zopflipng
        ${zopfli_SOURCE_DIR}/src/zopfli
        ${libmseed_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/third_party/MACLib/Shared
        ${CMAKE_SOURCE_DIR}/third_party/flexigif/core
        ${CMAKE_SOURCE_DIR}/third_party/stb
        ${taglib_SOURCE_DIR}
        ${taglib_BINARY_DIR}
        ${CMAKE_SOURCE_DIR}/third_party/bmplib
        ${CMAKE_BINARY_DIR}/bmplib
        ${sqlite3_SOURCE_DIR}
        ${libdeflate_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/third_party/libjxl/third_party/brotli/c/include
        ${CMAKE_SOURCE_DIR}/third_party/woff2/include
        ${CMAKE_SOURCE_DIR}/third_party/xz/src/liblzma/api
        ${ZSTD_INCLUDE_DIR}
        ${BZIP2_DIR}
        ${CMAKE_SOURCE_DIR}/third_party/kanzi-cpp/src
        ${CMAKE_SOURCE_DIR}/third_party/openjpeg/src/lib/openjp2
        ${CMAKE_BINARY_DIR}/third_party/openjpeg/src/lib/openjp2
)

if(NOT WIN32)
    target_include_directories(libchisel SYSTEM PRIVATE
            ${LIBMAGIC_INCLUDE_DIR}
            ${CMAKE_SOURCE_DIR}/third_party/gifsicle/include
            ${CMAKE_SOURCE_DIR}/third_party/gifsicle/src
    )
endif()
if(WIN32)
    set(MAGIC_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated_magic")
    file(MAKE_DIRECTORY ${MAGIC_GEN_DIR})

    add_custom_command(
            OUTPUT "${MAGIC_GEN_DIR}/magic_mgc_windows.h"
            COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_SOURCE_DIR}/scripts/gen_magic.ps1"
            -InputFile "${unofficial-libmagic_DICTIONARY}"
            -OutputFile "${MAGIC_GEN_DIR}/magic_mgc_windows.h"
            DEPENDS "${unofficial-libmagic_DICTIONARY}" "${CMAKE_SOURCE_DIR}/scripts/gen_magic.ps1"
            COMMENT "generating windows-specific magic_mgc.h via powershell"
    )

    add_custom_target(generate_windows_magic DEPENDS "${MAGIC_GEN_DIR}/magic_mgc_windows.h")
    add_dependencies(libchisel generate_windows_magic)

    target_include_directories(libchisel PRIVATE "${MAGIC_GEN_DIR}")
endif()
if(ENABLE_MATROSKA)
    target_include_directories(libchisel PRIVATE ${libebml_SOURCE_DIR}
            ${libmatroska_SOURCE_DIR}
            ${libmatroska_SOURCE_DIR}/src)
endif()
if(ENABLE_OPTIVORBIS)
    target_link_libraries(libchisel PRIVATE chisel_optivorbis_bridge)
endif()

target_link_libraries(libchisel PUBLIC ${CHISEL_LIBS})
# for cfbf processor
if(WIN32)
    target_link_libraries(libchisel PRIVATE ole32)
endif()

target_compile_features(libchisel PRIVATE cxx_std_20)
if(WIN32)
    target_compile_definitions(libchisel PUBLIC LIBARCHIVE_STATIC PLATFORM_WINDOWS)
elseif(APPLE)
    target_compile_definitions(libchisel PUBLIC PLATFORM_APPLE)
elseif(UNIX AND NOT APPLE)
    target_compile_definitions(libchisel PUBLIC PLATFORM_LINUX)
endif()
target_compile_definitions(libchisel PUBLIC TAGLIB_STATIC)

target_compile_definitions(libchisel PRIVATE LZMA_API_STATIC)

add_dependencies(libchisel zlibstatic)
add_dependencies(libchisel mozjpeg)
add_dependencies(libchisel liblzma)
add_dependencies(libchisel libzstd_static)
add_dependencies(libchisel png_static)
add_dependencies(libchisel ogg)
add_dependencies(libchisel FLAC)
add_dependencies(libchisel flexigif_core)
add_dependencies(libchisel wavpack)
add_dependencies(libchisel archive_static)
if(ENABLE_MATROSKA)
    add_dependencies(libchisel mkclean_lib)
endif()
add_dependencies(libchisel thirdparty_webp thirdparty_webpdemux thirdparty_webpmux)
add_dependencies(libchisel libqpdf)
add_dependencies(libchisel libzopfli libzopflipng)
add_dependencies(libchisel jxl jxl_threads)
add_dependencies(libchisel tiff)
add_dependencies(libchisel sqlite3-static)
add_dependencies(libchisel MAC)
add_dependencies(libchisel tag)
add_dependencies(libchisel pugixml)
add_dependencies(libchisel libdeflate_static)
add_dependencies(libchisel libgifsicle)
add_dependencies(libchisel mseed_static)
add_dependencies(libchisel brotlicommon brotlidec brotlienc)
add_dependencies(libchisel woff2common woff2dec woff2enc)
add_dependencies(libchisel libkanzi)
add_dependencies(libchisel openjp2)

if(NOT WIN32)
    add_dependencies(libchisel LIBMAGIC)
endif()