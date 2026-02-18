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
        set(LIB_EXT ".lib")
    else()
        set(LIB_EXT ".a")
    endif()

    set(OCAML_STDLIB_ASM "libasmrun${LIB_EXT}")
    set(OCAML_STDLIB_UNIX "libunix${LIB_EXT}")
    set(OCAML_STDLIB_STR "libcamlstr${LIB_EXT}")
    set(OCAML_STDLIB_THREADS "libthreadsnat${LIB_EXT}")

    set(BUILD_DIR "${MP3PACKER_ROOT}/_build/default")
    set(MP3_CMXA "${BUILD_DIR}/mp3packer.cmxa")

    set(GLUE_OBJ "${BUILD_DIR}/glue.o")

    add_custom_command(
            OUTPUT ${GLUE_OBJ}
            COMMAND ${DUNE_CMD} build --root ${MP3PACKER_ROOT} mp3packer.cmxa
            COMMAND ${OCAMLOPT_CMD} -thread -output-obj -o ${GLUE_OBJ}
            -I ${BUILD_DIR}
            -I +unix -I +str -I +threads
            -linkall
            unix.cmxa str.cmxa threads.cmxa
            ${MP3_CMXA}
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
            VERBATIM
    )

    add_custom_target(${TARGET_NAME}_build DEPENDS ${GLUE_OBJ})

    add_library(${TARGET_NAME} INTERFACE IMPORTED GLOBAL)
    add_dependencies(${TARGET_NAME} ${TARGET_NAME}_build)

    target_include_directories(${TARGET_NAME} INTERFACE ${OCAML_LIB_PATH})

    if(EXISTS "${OCAML_LIB_PATH}/threads/${OCAML_STDLIB_THREADS}")
        set(THREADS_LIB_FULL "${OCAML_LIB_PATH}/threads/${OCAML_STDLIB_THREADS}")
    else()
        set(THREADS_LIB_FULL "${OCAML_LIB_PATH}/${OCAML_STDLIB_THREADS}")
    endif()

    target_link_libraries(${TARGET_NAME} INTERFACE
            "${GLUE_OBJ}"
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