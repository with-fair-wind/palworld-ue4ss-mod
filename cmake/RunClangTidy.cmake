if(NOT CLANG_TIDY)
    message(FATAL_ERROR "RunClangTidy.cmake: CLANG_TIDY is not set")
endif()
if(NOT SOURCE_DIR)
    message(FATAL_ERROR "RunClangTidy.cmake: SOURCE_DIR is not set")
endif()
if(NOT BUILD_DIR)
    message(FATAL_ERROR "RunClangTidy.cmake: BUILD_DIR is not set")
endif()
if(NOT EXISTS "${BUILD_DIR}/compile_commands.json")
    message(FATAL_ERROR
        "RunClangTidy.cmake: ${BUILD_DIR}/compile_commands.json not found. "
        "Run cmake --preset ninja-msvc-x64 first.")
endif()
if(NOT MODE STREQUAL "fix" AND NOT MODE STREQUAL "check")
    message(FATAL_ERROR "RunClangTidy.cmake: MODE must be 'fix' or 'check'")
endif()

# clang-tidy defines __clang_analyzer__, which makes UE4SS include a PVS-Studio-only
# header that is not part of this build. Clang 22 also diagnoses an enum bitwise
# operation in a vendored Unreal header as an error. Keep project checks enabled,
# but make the third-party headers parse with the same intent as the MSVC build.
set(PALWORLD_TIDY_EXTRA_ARGS
    -extra-arg=-U__clang_analyzer__
    -extra-arg=-Wno-enum-enum-conversion
)
if(MODE STREQUAL "fix")
    list(APPEND PALWORLD_TIDY_EXTRA_ARGS -fix)
endif()
if(WARNINGS_AS_ERRORS)
    list(APPEND PALWORLD_TIDY_EXTRA_ARGS -warnings-as-errors=*)
endif()

string(REPLACE "\\" "/" PALWORLD_SOURCE_DIR_NORMALIZED "${SOURCE_DIR}")
set(PALWORLD_SOURCE_FILTER "${PALWORLD_SOURCE_DIR_NORMALIZED}/mods/.*")

if(NOT WIN32)
    get_filename_component(CLANG_TIDY_DIR "${CLANG_TIDY}" DIRECTORY)
    get_filename_component(CLANG_TIDY_NAME "${CLANG_TIDY}" NAME)
    string(REGEX REPLACE "^clang-tidy" "run-clang-tidy"
        RUN_CLANG_TIDY_NAME "${CLANG_TIDY_NAME}")
    set(RUN_CLANG_TIDY_CANDIDATE
        "${CLANG_TIDY_DIR}/${RUN_CLANG_TIDY_NAME}")

    if(EXISTS "${RUN_CLANG_TIDY_CANDIDATE}")
        include(ProcessorCount)
        ProcessorCount(PALWORLD_TIDY_JOBS)
        if(PALWORLD_TIDY_JOBS EQUAL 0)
            set(PALWORLD_TIDY_JOBS 4)
        endif()

        execute_process(
            COMMAND "${RUN_CLANG_TIDY_CANDIDATE}"
                -p "${BUILD_DIR}"
                -clang-tidy-binary "${CLANG_TIDY}"
                -j ${PALWORLD_TIDY_JOBS}
                -quiet
                ${PALWORLD_TIDY_EXTRA_ARGS}
                "${PALWORLD_SOURCE_FILTER}"
            COMMAND_ERROR_IS_FATAL ANY
        )
        return()
    endif()
endif()

file(READ "${BUILD_DIR}/compile_commands.json" PALWORLD_COMPILE_COMMANDS)
string(JSON PALWORLD_COMPILE_COMMANDS_LENGTH
    ERROR_VARIABLE json_error
    LENGTH "${PALWORLD_COMPILE_COMMANDS}")
if(json_error)
    message(FATAL_ERROR
        "RunClangTidy.cmake: failed to parse compile_commands.json: ${json_error}")
endif()

set(PALWORLD_TIDY_FILES "")
if(PALWORLD_COMPILE_COMMANDS_LENGTH GREATER 0)
    math(EXPR last_index "${PALWORLD_COMPILE_COMMANDS_LENGTH} - 1")
    foreach(index RANGE 0 ${last_index})
        string(JSON compile_file GET "${PALWORLD_COMPILE_COMMANDS}" ${index} file)
        if(NOT IS_ABSOLUTE "${compile_file}")
            string(JSON compile_directory
                GET "${PALWORLD_COMPILE_COMMANDS}" ${index} directory)
            file(REAL_PATH "${compile_file}" compile_file
                BASE_DIRECTORY "${compile_directory}")
        endif()

        string(REPLACE "\\" "/" normalized_file "${compile_file}")
        if(normalized_file MATCHES
                "^${PALWORLD_SOURCE_DIR_NORMALIZED}/mods/.*\\.(cc|cpp|cxx)$")
            list(APPEND PALWORLD_TIDY_FILES "${compile_file}")
        endif()
    endforeach()
endif()

list(REMOVE_DUPLICATES PALWORLD_TIDY_FILES)

if(NOT PALWORLD_TIDY_FILES)
    message(STATUS
        "No translation units under mods/ were found in compile_commands.json.")
    return()
endif()

execute_process(
    COMMAND "${CLANG_TIDY}"
        -p "${BUILD_DIR}"
        ${PALWORLD_TIDY_EXTRA_ARGS}
        ${PALWORLD_TIDY_FILES}
    COMMAND_ERROR_IS_FATAL ANY
)
