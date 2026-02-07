function(add_mp3packer_library TARGET_NAME MP3PACKER_ROOT)
    find_program(OPAM_EXE opam)
    set(OPAM_HINTS "")
    set(OPAM_BIN_DIR "")

    if(OPAM_EXE)
        execute_process(
                COMMAND ${OPAM_EXE} var bin
                OUTPUT_VARIABLE OPAM_BIN_OUTPUT
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET
        )

        if(OPAM_BIN_OUTPUT)
            string(STRIP "${OPAM_BIN_OUTPUT}" OPAM_BIN_DIR)
            file(TO_CMAKE_PATH "${OPAM_BIN_DIR}" OPAM_BIN_DIR)
            list(APPEND OPAM_HINTS "${OPAM_BIN_DIR}")
        endif()
    endif()

    find_program(DUNE_EXE dune HINTS ${OPAM_HINTS} REQUIRED)
    find_program(OCAMLOPT_EXE ocamlopt HINTS ${OPAM_HINTS} REQUIRED)

    if(WIN32)
        set(PATH_SEP ";")
    else()
        set(PATH_SEP ":")
    endif()

    if(OPAM_BIN_DIR)
        set(ENV_WRAPPER ${CMAKE_COMMAND} -E env "PATH=${OPAM_BIN_DIR}${PATH_SEP}$ENV{PATH}")
    else()
        set(ENV_WRAPPER ${CMAKE_COMMAND} -E env)
    endif()

    execute_process(
            COMMAND ${OCAMLOPT_EXE} -where
            OUTPUT_VARIABLE OCAML_LIB_PATH
            OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    file(TO_CMAKE_PATH "${OCAML_LIB_PATH}" OCAML_LIB_PATH)

    if(WIN32)
        set(OBJ_EXT ".obj")
        set(LIB_EXT ".lib")
        set(STATIC_LIB_EXT ".lib")
        set(OCAML_STDLIB_ASM "libasmrun${LIB_EXT}")
        set(OCAML_STDLIB_UNIX "libunix${LIB_EXT}")
        set(OCAML_STDLIB_STR "libcamlstr${LIB_EXT}")
        set(OCAML_STDLIB_THREADS "libthreadsnat${LIB_EXT}")
    else()
        set(OBJ_EXT ".o")
        set(LIB_EXT ".a")
        set(STATIC_LIB_EXT ".a")
        set(OCAML_STDLIB_ASM "libasmrun${LIB_EXT}")
        set(OCAML_STDLIB_UNIX "libunix${LIB_EXT}")
        set(OCAML_STDLIB_STR "libcamlstr${LIB_EXT}")
        set(OCAML_STDLIB_THREADS "libthreadsnat${LIB_EXT}")
    endif()

    set(BUILD_DIR "${MP3PACKER_ROOT}/_build/default")
    set(GLUE_OBJ "${BUILD_DIR}/glue${OBJ_EXT}")
    set(MP3_STUBS "${BUILD_DIR}/libmp3packer_stubs${STATIC_LIB_EXT}")
    set(MP3_CMXA "${BUILD_DIR}/mp3packer.cmxa")

    add_custom_command(
            OUTPUT ${GLUE_OBJ} ${MP3_STUBS}

            COMMAND ${ENV_WRAPPER} ${DUNE_EXE} build --root ${MP3PACKER_ROOT} mp3packer.cmxa libmp3packer_stubs${STATIC_LIB_EXT}

            COMMAND ${ENV_WRAPPER} ${OCAMLOPT_EXE} -thread -output-obj -o ${GLUE_OBJ}
            -I ${BUILD_DIR}
            -I +unix -I +str -I +threads
            -linkall
            unix.cmxa str.cmxa threads.cmxa
            ${MP3_CMXA}

            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
            COMMENT "Building OCaml mp3packer static library and glue..."
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

    message(STATUS "OCaml mp3packer configured. Runtime at: ${OCAML_LIB_PATH}")
endfunction()