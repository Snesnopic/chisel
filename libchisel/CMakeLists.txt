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
        include/jxl_processor.hpp
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
        src/processors/mkv_processor.cpp
        src/processors/mseed_processor.cpp
        src/processors/odf_processor.cpp
        src/processors/ooxml_processor.cpp
        src/processors/pdf_processor.cpp
        src/processors/png_processor.cpp
        src/processors/sqlite_processor.cpp
        src/processors/tiff_processor.cpp
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
)

add_library(libchisel STATIC ${LIBCHISEL_SOURCES})

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
        ${libebml_SOURCE_DIR}
        ${libmatroska_SOURCE_DIR}
        ${libmatroska_SOURCE_DIR}/src
        ${qpdf_SOURCE_DIR}/include
        ${qpdf_BINARY_DIR}/include
        ${zopfli_SOURCE_DIR}/include
        ${zopfli_BINARY_DIR}/include
        ${zopfli_SOURCE_DIR}/src/zopflipng
        ${zopfli_SOURCE_DIR}/src/zopfli
        ${libmseed_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/third_party/MACLib/Shared
        ${CMAKE_SOURCE_DIR}/third_party/flexigif
        ${CMAKE_SOURCE_DIR}/third_party/stb
        ${taglib_SOURCE_DIR}
        ${taglib_BINARY_DIR}
        ${CMAKE_SOURCE_DIR}/third_party/bmplib
        ${CMAKE_BINARY_DIR}/bmplib
)

if(NOT WIN32)
    target_include_directories(libchisel SYSTEM PRIVATE
            ${LIBMAGIC_INCLUDE_DIR}
            ${CMAKE_SOURCE_DIR}/third_party/gifsicle/include
            ${CMAKE_SOURCE_DIR}/third_party/gifsicle/src
    )
endif()

target_link_libraries(libchisel PUBLIC ${CHISEL_LIBS})

target_compile_features(libchisel PRIVATE cxx_std_23)
if(WIN32)
    target_compile_definitions(libchisel PUBLIC LIBARCHIVE_STATIC PLATFORM_WINDOWS)
elseif(APPLE)
    target_compile_definitions(libchisel PUBLIC PLATFORM_APPLE)
elseif(UNIX AND NOT APPLE)
    target_compile_definitions(libchisel PUBLIC PLATFORM_LINUX)
endif()
target_compile_definitions(libchisel PUBLIC TAGLIB_STATIC)

add_dependencies(libchisel zlibstatic)
add_dependencies(libchisel mozjpeg)
add_dependencies(libchisel mkclean_lib)
add_dependencies(libchisel liblzma)
add_dependencies(libchisel libzstd_static)
add_dependencies(libchisel png_static)
add_dependencies(libchisel ogg)
add_dependencies(libchisel FLAC)
add_dependencies(libchisel flexigif)
add_dependencies(libchisel wavpack)
add_dependencies(libchisel archive_static)
add_dependencies(libchisel ebml)
add_dependencies(libchisel matroska)
add_dependencies(libchisel thirdparty_webp thirdparty_webpdecoder thirdparty_webpdemux thirdparty_webpmux)
add_dependencies(libchisel libqpdf)
add_dependencies(libchisel libzopfli libzopflipng)
add_dependencies(libchisel jxl jxl_threads)
add_dependencies(libchisel tiff)
add_dependencies(libchisel SQLite::SQLite3)
add_dependencies(libchisel MAC)
add_dependencies(libchisel tag)

if(NOT WIN32)
    add_dependencies(libchisel libmagic)
    add_dependencies(libchisel libmseed)
    add_dependencies(libchisel giflib)
endif()