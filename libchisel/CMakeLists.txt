set(LIBCHISEL_SOURCES
        include/ape_processor.hpp
        include/archive_processor.hpp
        include/asf_processor.hpp
        include/avi_processor.hpp
        include/event_bus.hpp
        include/events.hpp
        include/file_type.hpp
        include/flac_processor.hpp
        include/flacout_processor.hpp
        include/jpeg_processor.hpp
        include/mime_processor.hpp
        src/processors/mime_processor.cpp
        include/pe_processor.hpp
        include/jxl_processor.hpp
        include/kanzi_processor.hpp
        include/vcf_processor.hpp
        include/json_processor.hpp
        include/mng_processor.hpp
        include/mp4_processor.hpp
        include/optigif_processor.hpp
        include/pcx_processor.hpp
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
        include/sparse_rewrite_util.hpp
        include/sqlite_processor.hpp
        include/thread_pool.hpp
        include/tiff_processor.hpp
        include/wavpack_processor.hpp
        include/webp_processor.hpp
        include/zopflipng_processor.hpp
        src/processors/ape_processor.cpp
        src/processors/archive_processor.cpp
        src/processors/asf_processor.cpp
        src/processors/avi_processor.cpp
        src/processors/flac_processor.cpp
        src/processors/flacout_processor.cpp
        src/processors/jpeg_processor.cpp
        src/processors/jxl_processor.cpp
        src/processors/kanzi_processor.cpp
        src/processors/mkv_processor.cpp
        src/processors/mp4_processor.cpp
        src/processors/optigif_processor.cpp
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
        src/processors/mng_processor.cpp
        src/processors/pcx_processor.cpp
        src/processors/jp2_processor.cpp
        src/processors/wavpack_processor.cpp
        src/processors/webp_processor.cpp
        src/processors/zopflipng_processor.cpp
        src/utils/logger.cpp
        src/utils/mime_detector.cpp
        src/utils/processor_executor.cpp
        src/utils/processor_registry.cpp
        src/utils/random_utils.cpp
        src/utils/sparse_rewrite_util.cpp
        src/utils/thread_pool.cpp
        include/tga_processor.hpp
        src/processors/tga_processor.cpp
        include/file_utils.hpp
        src/utils/file_utils.cpp
        include/zip_extract_util.hpp
        src/utils/zip_extract_util.cpp
        include/audio_metadata_util.hpp
        src/utils/audio_metadata_util.cpp
        src/utils/base64_utils.cpp
        include/mpeg_processor.hpp
        src/processors/mpeg_processor.cpp
        include/ogg_processor.hpp
        src/processors/ogg_processor.cpp
        include/tag_processor.hpp
        src/processors/tag_processor.cpp
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
        include/gz_processor.hpp
        src/processors/gz_processor.cpp
        include/lua_processor.hpp
        src/processors/lua_processor.cpp
        include/stl_processor.hpp
        src/processors/stl_processor.cpp
        include/cab_processor.hpp
        src/processors/cab_processor.cpp
        include/lz4_processor.hpp
        src/processors/lz4_processor.cpp

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
        ${CMAKE_SOURCE_DIR}/third_party/flacoutcpp/include
        ${CMAKE_SOURCE_DIR}/third_party/optigif/include
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
        ${CMAKE_SOURCE_DIR}/third_party/lz4/lib
)

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
add_dependencies(libchisel lz4_static)
add_dependencies(libchisel libzstd_static)
add_dependencies(libchisel png_static)
add_dependencies(libchisel ogg)
add_dependencies(libchisel FLAC)
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
add_dependencies(libchisel mseed_static)
add_dependencies(libchisel brotlicommon brotlidec brotlienc)
add_dependencies(libchisel woff2common woff2dec woff2enc)
add_dependencies(libchisel libkanzi)
add_dependencies(libchisel openjp2)
