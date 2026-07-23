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

cmake_minimum_required(VERSION 3.22)

project(PalworldMods LANGUAGES CXX)

include(CTest)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(PalworldTooling)

# Guard: RE-UE4SS must be cloned (by scripts/setup.ps1) before configuring.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/RE-UE4SS/CMakeLists.txt")
    message(FATAL_ERROR
        "RE-UE4SS 未出现在 '${CMAKE_CURRENT_SOURCE_DIR}/RE-UE4SS'。\n"
        "请先运行首次初始化：  pwsh scripts/setup.ps1")
endif()

# Pull in the UE4SS framework. This defines the `UE4SS` target we link against,
# plus its `deps/`. We only ever build our own mod target, so the separate UVTD
# program (and its Rust toolchain) is never compiled.
add_subdirectory(RE-UE4SS)

# The actual mods. Add more subdirectories here as the project grows.
add_subdirectory(mods)
