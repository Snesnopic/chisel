function(add_mp3packer_library TARGET_NAME MP3PACKER_ROOT)
    find_program(OPAM_EXE opam REQUIRED)

    set(DUNE_CMD ${OPAM_EXE} exec -- dune)
    set(OCAMLOPT_CMD ${OPAM_EXE} exec -- ocamlopt)

    set(BUILD_DIR "${MP3PACKER_ROOT}/_build/default")
    set(GLUE_OBJ "${BUILD_DIR}/glue.o")

    set(DIAGNOSTIC_SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/diagnose.cmake")
    file(WRITE "${DIAGNOSTIC_SCRIPT}" "
file(GLOB ALL_LIBS \"${BUILD_DIR}/*.a\" \"${BUILD_DIR}/*.lib\")
message(WARNING \"\\n\\n=============================================================\")
message(WARNING \"FILE DI LIBRERIA GENERATI DA DUNE NELLA CARTELLA:\")
foreach(f \${ALL_LIBS})
    message(WARNING \"---> \${f}\")
endforeach()
message(WARNING \"=============================================================\\n\\n\")
")

    add_custom_command(
            OUTPUT ${GLUE_OBJ}
            COMMAND ${DUNE_CMD} build --root ${MP3PACKER_ROOT} mp3packer.cmxa
            COMMAND ${CMAKE_COMMAND} -P "${DIAGNOSTIC_SCRIPT}"
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
            VERBATIM
    )

    add_custom_target(${TARGET_NAME}_build DEPENDS ${GLUE_OBJ})

    add_library(${TARGET_NAME} STATIC IMPORTED GLOBAL)
    add_dependencies(${TARGET_NAME} ${TARGET_NAME}_build)

    set_target_properties(${TARGET_NAME} PROPERTIES
            IMPORTED_LOCATION "${GLUE_OBJ}"
    )
endfunction()