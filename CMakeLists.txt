# Root super-build for the Palworld UE4SS C++ mod(s).
#
# Layout (see scripts/setup.ps1):
#   Palworld/
#   ├── CMakeLists.txt        (this file)
#   ├── RE-UE4SS/             cloned dependency — supplies the `UE4SS` target
#   └── mods/                 one subdirectory per mod
#
# The mod targets link the `UE4SS` static library, which transitively provides all
# headers, compile flags, and definitions (including the C++23 language standard).

cmake_minimum_required(VERSION 3.26)

find_package(Git REQUIRED)
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_SOURCE_DIR}/RE-UE4SS" rev-parse HEAD
    OUTPUT_VARIABLE palworld_dependency_commit OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE palworld_git_result)
if(NOT palworld_git_result EQUAL 0)
    message(FATAL_ERROR "Run pwsh scripts/setup.ps1 before configuring.")
endif()
if(DEFINED PALWORLD_CONFIGURED_UE4SS_COMMIT AND
   NOT PALWORLD_CONFIGURED_UE4SS_COMMIT STREQUAL palworld_dependency_commit)
    message(FATAL_ERROR "UE4SS changed. Select a new build directory; keep the old artifacts.")
endif()
set(PALWORLD_CONFIGURED_UE4SS_COMMIT "${palworld_dependency_commit}" CACHE INTERNAL "Configured UE4SS identity")

project(PalworldMods LANGUAGES CXX)

include(CTest)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL "Do not advance already populated third-party dependencies during normal builds")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(PalworldTooling)

# Guard: RE-UE4SS must be cloned (by scripts/setup.ps1) before configuring.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/RE-UE4SS/CMakeLists.txt")
    message(FATAL_ERROR
        "RE-UE4SS 未出现在 '${CMAKE_CURRENT_SOURCE_DIR}/RE-UE4SS'。\n"
        "请先运行首次初始化：  pwsh scripts/setup.ps1")
endif()

# Pull in the UE4SS framework. This defines the `UE4SS` target we link against,
# plus its dependencies. The standalone UVTD program is not built, but the
# current UE4SS target still builds its Rust-backed PatternSleuth dependency.
add_subdirectory(RE-UE4SS)

# The actual mods. Add more subdirectories here as the project grows.
add_subdirectory(mods)
