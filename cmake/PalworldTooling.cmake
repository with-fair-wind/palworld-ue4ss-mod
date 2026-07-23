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

option(PALWORLD_CLANG_TIDY_WARNINGS_AS_ERRORS
    "Treat clang-tidy diagnostics as errors in tidy-check"
    OFF
)

get_property(PALWORLD_IS_MULTI_CONFIG
    GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)

find_program(PALWORLD_CLANG_TIDY_EXECUTABLE
    NAMES clang-tidy-22 clang-tidy-21 clang-tidy-20 clang-tidy-19 clang-tidy-18
          clang-tidy
    DOC "clang-tidy binary used by the tidy targets"
)

if(PALWORLD_CLANG_TIDY_EXECUTABLE AND NOT PALWORLD_IS_MULTI_CONFIG)
    set(PALWORLD_RUN_CLANG_TIDY
        "${CMAKE_SOURCE_DIR}/cmake/RunClangTidy.cmake")

    add_custom_target(tidy
        COMMAND ${CMAKE_COMMAND}
            "-DCLANG_TIDY=${PALWORLD_CLANG_TIDY_EXECUTABLE}"
            "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
            "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
            "-DMODE=fix"
            "-DWARNINGS_AS_ERRORS=OFF"
            -P "${PALWORLD_RUN_CLANG_TIDY}"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Running clang-tidy --fix over translation units under mods/"
        VERBATIM
        USES_TERMINAL
    )

    add_custom_target(tidy-check
        COMMAND ${CMAKE_COMMAND}
            "-DCLANG_TIDY=${PALWORLD_CLANG_TIDY_EXECUTABLE}"
            "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
            "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
            "-DMODE=check"
            "-DWARNINGS_AS_ERRORS=${PALWORLD_CLANG_TIDY_WARNINGS_AS_ERRORS}"
            -P "${PALWORLD_RUN_CLANG_TIDY}"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Running clang-tidy over translation units under mods/"
        VERBATIM
        USES_TERMINAL
    )
else()
    message(STATUS
        "clang-tidy is unavailable or the generator is multi-config; "
        "tidy targets are disabled.")
endif()
