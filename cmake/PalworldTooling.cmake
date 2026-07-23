include_guard(GLOBAL)

find_package(Git QUIET)

find_program(PALWORLD_CLANG_FORMAT_EXECUTABLE
    NAMES clang-format-22 clang-format-21 clang-format-20 clang-format-19 clang-format-18
          clang-format
    DOC "clang-format binary used by the format targets"
)

if(PALWORLD_CLANG_FORMAT_EXECUTABLE
        AND GIT_FOUND
        AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    set(PALWORLD_RUN_CLANG_FORMAT
        "${CMAKE_SOURCE_DIR}/cmake/RunClangFormat.cmake")

    add_custom_target(format
        COMMAND ${CMAKE_COMMAND}
            "-DCLANG_FORMAT=${PALWORLD_CLANG_FORMAT_EXECUTABLE}"
            "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
            "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
            "-DMODE=fix"
            -P "${PALWORLD_RUN_CLANG_FORMAT}"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Formatting tracked C/C++ files under mods/"
        VERBATIM
        USES_TERMINAL
    )

    add_custom_target(format-check
        COMMAND ${CMAKE_COMMAND}
            "-DCLANG_FORMAT=${PALWORLD_CLANG_FORMAT_EXECUTABLE}"
            "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
            "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
            "-DMODE=check"
            -P "${PALWORLD_RUN_CLANG_FORMAT}"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Checking tracked C/C++ formatting under mods/"
        VERBATIM
        USES_TERMINAL
    )
else()
    message(STATUS
        "clang-format, Git, or a Git checkout is unavailable; "
        "format targets are disabled.")
endif()
