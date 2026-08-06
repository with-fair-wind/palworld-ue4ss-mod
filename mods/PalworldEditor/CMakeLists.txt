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
    src/mod/dllmain.cpp
    src/mod/editor_ui.cpp
    src/items/item_ui.cpp
    src/skills/pal_skills.cpp
    src/skills/skill_ui.cpp
    src/grappling_hook/grapple_cooldown_gateway.cpp
    src/base_resource_sharing/pal_base_resource_runtime.cpp
    src/base_resource_sharing/pal_base_resources.cpp
    src/base_resource_sharing/base_resource_ui.cpp
    src/pal_identity/pal_identity.cpp
    src/pal_identity/pal_identity_ui.cpp
    src/pal_stats/pal_stats.cpp
    src/pal_stats/stat_ui.cpp
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

    add_executable(PalworldEditorBaseResourceSharingTests
        tests/base_resource_sharing_tests.cpp
    )
    target_include_directories(PalworldEditorBaseResourceSharingTests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc)
    target_compile_features(PalworldEditorBaseResourceSharingTests PRIVATE cxx_std_23)
    if(MSVC)
        target_compile_options(PalworldEditorBaseResourceSharingTests
            PRIVATE /utf-8 /W4 /permissive-)
    endif()
    add_test(NAME PalworldEditor.BaseResourceSharing
        COMMAND PalworldEditorBaseResourceSharingTests)

    add_executable(PalworldEditorRemotePalboxTests
        tests/remote_palbox_tests.cpp
    )
    target_include_directories(PalworldEditorRemotePalboxTests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc)
    target_compile_features(PalworldEditorRemotePalboxTests PRIVATE cxx_std_23)
    if(MSVC)
        target_compile_options(PalworldEditorRemotePalboxTests
            PRIVATE /utf-8 /W4 /permissive-)
    endif()
    add_test(NAME PalworldEditor.RemotePalbox
        COMMAND PalworldEditorRemotePalboxTests)
endif()

# One-click deploy into the Palworld game install (see cmake/Deploy.cmake).
include(${CMAKE_SOURCE_DIR}/cmake/Deploy.cmake)
palworld_add_deploy_target(${TARGET})
