# cmake/EBMLConfig.cmake
if(TARGET ebml)
    if(NOT TARGET EBML::ebml)
        add_library(EBML::ebml ALIAS ebml)
    endif()
else()
    message(FATAL_ERROR "Target 'ebml' not found: add libebml before libmatroska")
endif()