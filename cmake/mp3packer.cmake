function(add_mp3packer_library TARGET_NAME MP3PACKER_ROOT)
    find_program(OPAM_EXE opam REQUIRED)

    set(DUNE_CMD ${OPAM_EXE} exec -- dune)
    set(OCAMLOPT_CMD ${OPAM_EXE} exec -- ocamlopt)

    execute_process(
            COMMAND ${OCAMLOPT_CMD} -where
            OUTPUT_VARIABLE OCAML_LIB_PATH
            OUTPUT_STRIP_TRAILING_WHITESPACE
            COMMAND_ERROR_IS_FATAL ANY
    )
    string(STRIP "${OCAML_LIB_PATH}" OCAML_LIB_PATH)
    file(TO_CMAKE_PATH "${OCAML_LIB_PATH}" OCAML_LIB_PATH)

    if(WIN32)
        set(OBJ_EXT ".o")
        set(MP3_STUBS_FILE "libmp3packer_stubs.a")
        set(DUNE_TARGETS "mp3packer.cmxa")
    else()
        set(OBJ_EXT ".o")
        set(MP3_STUBS_FILE "libmp3packer_stubs.a")
        set(DUNE_TARGETS "mp3packer.cmxa" "${MP3_STUBS_FILE}")
    endif()

    set(BUILD_DIR "${MP3PACKER_ROOT}/_build/default")
    set(GLUE_OBJ "${BUILD_DIR}/glue${OBJ_EXT}")
    set(MP3_STUBS "${BUILD_DIR}/${MP3_STUBS_FILE}")
    set(MP3_CMXA "${BUILD_DIR}/mp3packer.cmxa")

    set(OCAML_SYS_LIBS "")
    foreach(LIB_BASENAME unix camlstr threadsnat asmrun)
        set(FOUND_LIB "NOTFOUND")
        foreach(SEARCH_PATH "${OCAML_LIB_PATH}" "${OCAML_LIB_PATH}/threads")
            foreach(PREFIX "lib" "")
                foreach(EXT ".a" ".lib")
                    set(CANDIDATE "${SEARCH_PATH}/${PREFIX}${LIB_BASENAME}${EXT}")
                    if(EXISTS "${CANDIDATE}" AND FOUND_LIB STREQUAL "NOTFOUND")
                        set(FOUND_LIB "${CANDIDATE}")
                    endif()
                endforeach()
            endforeach()
        endforeach()

        if(FOUND_LIB STREQUAL "NOTFOUND")
            message(FATAL_ERROR "Impossibile trovare la libreria di sistema OCaml: ${LIB_BASENAME}")
        else()
            list(APPEND OCAML_SYS_LIBS "${FOUND_LIB}")
        endif()
    endforeach()

    add_custom_command(
            OUTPUT ${GLUE_OBJ} ${MP3_STUBS}
            COMMAND ${DUNE_CMD} build --root ${MP3PACKER_ROOT} ${DUNE_TARGETS}
            COMMAND ${OCAMLOPT_CMD} -thread -output-obj -o ${GLUE_OBJ}
            -I ${BUILD_DIR}
            -I +unix -I +str -I +threads
            -linkall
            unix.cmxa str.cmxa threads.cmxa
            ${MP3_CMXA}
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
            VERBATIM
    )

    add_custom_target(${TARGET_NAME}_build DEPENDS ${GLUE_OBJ} ${MP3_STUBS})

    add_library(${TARGET_NAME} STATIC IMPORTED GLOBAL)
    add_dependencies(${TARGET_NAME} ${TARGET_NAME}_build)

    set_target_properties(${TARGET_NAME} PROPERTIES
            IMPORTED_LOCATION "${MP3_STUBS}"
            INTERFACE_LINK_LIBRARIES "${GLUE_OBJ}"
    )

    target_include_directories(${TARGET_NAME} INTERFACE ${OCAML_LIB_PATH})

    target_link_libraries(${TARGET_NAME} INTERFACE
            ${OCAML_SYS_LIBS}
    )

    if(WIN32)
        target_link_libraries(${TARGET_NAME} INTERFACE ws2_32 iphlpapi)
    elseif(APPLE)
    else()
        target_link_libraries(${TARGET_NAME} INTERFACE pthread dl m)
    endif()

    message(STATUS "OCaml mp3packer configured via opam. Runtime at: ${OCAML_LIB_PATH}")
endfunction()