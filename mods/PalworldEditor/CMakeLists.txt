# PalworldEditor — a single UE4SS C++ mod target.
#
# To add another mod: copy this folder, change TARGET, and add_subdirectory() it
# from a parent CMakeLists.txt.

cmake_minimum_required(VERSION 3.22)

set(TARGET PalworldEditor)
project(${TARGET} LANGUAGES CXX)

# Match RE-UE4SS's language settings (defensive — UE4SS already sets these
# globally, but we restate them so this target is correct in isolation).
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(${TARGET} SHARED
    src/dllmain.cpp
    src/pal_skills.cpp
)

target_include_directories(${TARGET} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc)

# UE4SS supplies all headers, compile flags, and definitions transitively.
target_link_libraries(${TARGET} PRIVATE UE4SS)

# Output name = PalworldEditor (DLL: PalworldEditor.dll; UE4SS installs it as dlls/main.dll).
set_target_properties(${TARGET} PROPERTIES OUTPUT_NAME ${TARGET})

if(BUILD_TESTING)
    add_executable(PalworldEditorTests
        tests/skill_editor_tests.cpp
    )
    target_include_directories(PalworldEditorTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc)
    target_compile_features(PalworldEditorTests PRIVATE cxx_std_23)
    if(MSVC)
        target_compile_options(PalworldEditorTests PRIVATE /utf-8 /W4 /permissive-)
    endif()
    add_test(NAME PalworldEditor.SkillEditor COMMAND PalworldEditorTests)
endif()

# One-click deploy into the Palworld game install (see cmake/Deploy.cmake).
include(${CMAKE_SOURCE_DIR}/cmake/Deploy.cmake)
palworld_add_deploy_target(${TARGET})
