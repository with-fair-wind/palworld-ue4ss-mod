if(NOT CLANG_FORMAT)
    message(FATAL_ERROR "RunClangFormat.cmake: CLANG_FORMAT is not set")
endif()
if(NOT GIT_EXECUTABLE)
    message(FATAL_ERROR "RunClangFormat.cmake: GIT_EXECUTABLE is not set")
endif()
if(NOT SOURCE_DIR)
    message(FATAL_ERROR "RunClangFormat.cmake: SOURCE_DIR is not set")
endif()
if(NOT MODE STREQUAL "fix" AND NOT MODE STREQUAL "check")
    message(FATAL_ERROR "RunClangFormat.cmake: MODE must be 'fix' or 'check'")
endif()

execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" ls-files --
        "*.c" "*.cc" "*.cpp" "*.cxx" "*.h" "*.hpp" "*.hxx"
    OUTPUT_VARIABLE PALWORLD_FORMAT_FILES
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY
)

if(PALWORLD_FORMAT_FILES STREQUAL "")
    message(STATUS "No tracked C/C++ files found; nothing to format.")
    return()
endif()

string(REPLACE "\r\n" "\n" PALWORLD_FORMAT_FILES "${PALWORLD_FORMAT_FILES}")
string(REPLACE "\n" ";" PALWORLD_FORMAT_FILES "${PALWORLD_FORMAT_FILES}")

set(PALWORLD_FORMAT_ABS_FILES "")
foreach(file_path IN LISTS PALWORLD_FORMAT_FILES)
    string(REPLACE "\\" "/" normalized_path "${file_path}")
    if(NOT normalized_path MATCHES "^mods/")
        continue()
    endif()
    list(APPEND PALWORLD_FORMAT_ABS_FILES "${SOURCE_DIR}/${normalized_path}")
endforeach()

if(NOT PALWORLD_FORMAT_ABS_FILES)
    message(STATUS "No tracked C/C++ files under mods/; nothing to format.")
    return()
endif()

if(MODE STREQUAL "fix")
    execute_process(
        COMMAND "${CLANG_FORMAT}" -i --style=file ${PALWORLD_FORMAT_ABS_FILES}
        COMMAND_ERROR_IS_FATAL ANY
    )
else()
    execute_process(
        COMMAND "${CLANG_FORMAT}" --dry-run --Werror --style=file
            ${PALWORLD_FORMAT_ABS_FILES}
        COMMAND_ERROR_IS_FATAL ANY
    )
endif()
