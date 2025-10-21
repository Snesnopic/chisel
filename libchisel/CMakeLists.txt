# libchisel/CMakeLists.txt

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
        # Aggiungi gif_processor condizionalmente
        src/utils/logger.cpp
        src/utils/mime_detector.cpp
        src/utils/processor_executor.cpp
        src/utils/processor_registry.cpp
        src/utils/random_utils.cpp
        src/utils/thread_pool.cpp
)
if(NOT WIN32)
    list(APPEND LIBCHISEL_SOURCES src/processors/gif_processor.cpp)
endif()

# --- Definizione Libreria ---
add_library(libchisel STATIC ${LIBCHISEL_SOURCES})

# --- Include Directories ---
target_include_directories(libchisel
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        # Aggiungi qui gli include delle librerie terze USATI DIRETTAMENTE
        # negli header pubblici di libchisel (es. processor.hpp se include tipi esterni)
        # Esempio: ${libpng_SOURCE_DIR}
        PRIVATE
        src
        # Aggiungi qui include di librerie terze usati SOLO nei .cpp di libchisel
        # Esempio: ${JPEG_INCLUDE_DIR}
        ${libflac_SOURCE_DIR}/include
        ${libflac_BINARY_DIR}/include
        ${libpng_SOURCE_DIR}
        ${libpng_BINARY_DIR}
        # ${MOZJPEG_INCLUDE_DIR} # Probabilmente non serve, mozjpeg è header-only? Verifica
        ${JPEG_INCLUDE_DIR}
        ${libarchive_SOURCE_DIR}/libarchive
        ${libarchive_BINARY_DIR}/libarchive
        ${libogg_SOURCE_DIR}/include
        ${libogg_BINARY_DIR}/include
        ${libebml_SOURCE_DIR}
        ${libmatroska_SOURCE_DIR}/src
        ${qpdf_SOURCE_DIR}/include
        ${qpdf_BINARY_DIR}/include
        ${zopfli_SOURCE_DIR}/include
        ${zopfli_BINARY_DIR}/include
        ${zopfli_SOURCE_DIR}/src/zopflipng # Per zopflipng_processor
        ${zopfli_SOURCE_DIR}/src/zopfli    # Per zopflipng_processor
        ${libmseed_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/third_party/MACLib/Shared
        ${CMAKE_SOURCE_DIR}/third_party/flexigif
)
if(NOT WIN32)
    target_include_directories(libchisel PRIVATE
            ${LIBMAGIC_INCLUDE_DIR} # Per mime_detector
            ${CMAKE_SOURCE_DIR}/third_party/gifsicle/include # Per GifProcessor
            ${CMAKE_SOURCE_DIR}/third_party/gifsicle/src     # Per GifProcessor
    )
endif()


target_link_libraries(libchisel PUBLIC ${CHISEL_LIBS})

target_compile_features(libchisel PRIVATE cxx_std_23)
if(WIN32)
    target_compile_definitions(libchisel PUBLIC LIBARCHIVE_STATIC PLATFORM_WINDOWS)
elseif(APPLE)
    target_compile_definitions(libchisel PUBLIC PLATFORM_APPLE)
elseif(UNIX AND NOT APPLE) # Assumendo Linux/Altri Unix
    target_compile_definitions(libchisel PUBLIC PLATFORM_LINUX)
endif()

# --- Dipendenze Build ---
# Assicurati che le librerie terze siano costruite prima di libchisel
add_dependencies(libchisel mozjpeg ${ZLIB_TARGET}) # Esempio ZLIB_TARGET da verificare
add_dependencies(libchisel mkclean_lib)
add_dependencies(libchisel liblzma)
add_dependencies(libchisel libzstd_static) # Nome target zstd
add_dependencies(libchisel png_static) # Nome target libpng
add_dependencies(libchisel ogg)        # Nome target libogg
add_dependencies(libchisel FLAC)       # Nome target libflac
add_dependencies(libchisel flexigif)
add_dependencies(libchisel wavpack)
add_dependencies(libchisel archive_static) # Nome target libarchive
add_dependencies(libchisel ebml)       # Nome target libebml
add_dependencies(libchisel matroska)   # Nome target libmatroska
add_dependencies(libchisel thirdparty_webp thirdparty_webpdecoder thirdparty_webpdemux thirdparty_webpmux) # Target libwebp
add_dependencies(libchisel libqpdf)     # Nome target qpdf
add_dependencies(libchisel libzopfli libzopflipng) # Target zopfli
add_dependencies(libchisel jxl jxl_threads) # Target libjxl
add_dependencies(libchisel tiff)       # Nome target libtiff
add_dependencies(libchisel SQLite::SQLite3) # Target sqlite3
add_dependencies(libchisel MAC)        # Nome target MACLib

if(NOT WIN32)
    add_dependencies(libchisel libmagic) # Dipendenza da ExternalProject libmagic
    add_dependencies(libchisel embed_magic_header) # Target definito nel root
    add_dependencies(libchisel libmseed) # Dipendenza da ExternalProject libmseed
    add_dependencies(libchisel giflib)   # Libreria Gifsicle
endif()

# End of file (libchisel)