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
        set(LIB_EXT ".lib")
        set(OCAML_STDLIB_ASM "libasmrun${LIB_EXT}")
        set(OCAML_STDLIB_UNIX "libunix${LIB_EXT}")
        set(OCAML_STDLIB_STR "libcamlstr${LIB_EXT}")
        set(OCAML_STDLIB_THREADS "libthreadsnat${LIB_EXT}")
        set(MP3_STUBS_FILE "libmp3packer_stubs.lib")
        set(DUNE_TARGETS "mp3packer.cmxa")
    else()
        set(OBJ_EXT ".o")
        set(LIB_EXT ".a")
        set(OCAML_STDLIB_ASM "libasmrun${LIB_EXT}")
        set(OCAML_STDLIB_UNIX "libunix${LIB_EXT}")
        set(OCAML_STDLIB_STR "libcamlstr${LIB_EXT}")
        set(OCAML_STDLIB_THREADS "libthreadsnat${LIB_EXT}")
        set(MP3_STUBS_FILE "libmp3packer_stubs.a")
        set(DUNE_TARGETS "mp3packer.cmxa" "${MP3_STUBS_FILE}")
    endif()

    set(BUILD_DIR "${MP3PACKER_ROOT}/_build/default")
    set(GLUE_OBJ "${BUILD_DIR}/glue${OBJ_EXT}")
    set(MP3_STUBS "${BUILD_DIR}/${MP3_STUBS_FILE}")
    set(MP3_CMXA "${BUILD_DIR}/mp3packer.cmxa")

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

    if(EXISTS "${OCAML_LIB_PATH}/threads/${OCAML_STDLIB_THREADS}")
        set(THREADS_LIB_FULL "${OCAML_LIB_PATH}/threads/${OCAML_STDLIB_THREADS}")
    else()
        set(THREADS_LIB_FULL "${OCAML_LIB_PATH}/${OCAML_STDLIB_THREADS}")
    endif()

    target_link_libraries(${TARGET_NAME} INTERFACE
            "${OCAML_LIB_PATH}/${OCAML_STDLIB_UNIX}"
            "${OCAML_LIB_PATH}/${OCAML_STDLIB_STR}"
            "${THREADS_LIB_FULL}"
            "${OCAML_LIB_PATH}/${OCAML_STDLIB_ASM}"
    )

    if(WIN32)
        target_link_libraries(${TARGET_NAME} INTERFACE ws2_32 iphlpapi)
    elseif(APPLE)
    else()
        target_link_libraries(${TARGET_NAME} INTERFACE pthread dl m)
    endif()

    message(STATUS "OCaml mp3packer configured via opam. Runtime at: ${OCAML_LIB_PATH}")
endfunction()