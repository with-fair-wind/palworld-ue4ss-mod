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
    src/common/function_hook_registry.cpp
    src/game/pal_base_camp_reflection.cpp
    src/items/item_ui.cpp
    src/items/stack_limit_gateway.cpp
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
    src/pal_remote_palbox/remote_palbox_runtime.cpp
    src/pal_remote_palbox/remote_palbox_ui.cpp
    src/pal_revive/pal_revive.cpp
    src/capture_override/capture_override_runtime.cpp
    src/capture_override/capture_override_ui.cpp
    src/revive_timer/revive_timer_gateway.cpp
    src/revive_timer/revive_timer_ui.cpp
    src/waypoint_teleport/waypoint_teleport_runtime.cpp
    src/waypoint_teleport/waypoint_teleport_ui.cpp
    src/game_settings/game_settings_gateway.cpp
    src/game_settings/game_settings_ui.cpp
    src/fishing_boost/fishing_boost_gateway.cpp
    src/fishing_boost/fishing_boost_ui.cpp
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

    add_executable(PalworldEditorCaptureOverrideTests
        tests/capture_override_tests.cpp
    )
    target_include_directories(PalworldEditorCaptureOverrideTests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc)
    target_compile_features(PalworldEditorCaptureOverrideTests PRIVATE cxx_std_23)
    if(MSVC)
        target_compile_options(PalworldEditorCaptureOverrideTests
            PRIVATE /utf-8 /W4 /permissive-)
    endif()
    add_test(NAME PalworldEditor.CaptureOverride
        COMMAND PalworldEditorCaptureOverrideTests)

    add_executable(PalworldEditorReviveTimerTests
        tests/revive_timer_tests.cpp
    )
    target_include_directories(PalworldEditorReviveTimerTests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc)
    target_compile_features(PalworldEditorReviveTimerTests PRIVATE cxx_std_23)
    if(MSVC)
        target_compile_options(PalworldEditorReviveTimerTests
            PRIVATE /utf-8 /W4 /permissive-)
    endif()
    add_test(NAME PalworldEditor.ReviveTimer
        COMMAND PalworldEditorReviveTimerTests)

    add_executable(PalworldEditorWaypointTeleportTests
        tests/waypoint_teleport_tests.cpp
    )
    target_include_directories(PalworldEditorWaypointTeleportTests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc)
    target_compile_features(PalworldEditorWaypointTeleportTests PRIVATE cxx_std_23)
    if(MSVC)
        target_compile_options(PalworldEditorWaypointTeleportTests
            PRIVATE /utf-8 /W4 /permissive-)
    endif()
    add_test(NAME PalworldEditor.WaypointTeleport
        COMMAND PalworldEditorWaypointTeleportTests)
endif()

# One-click deploy into the Palworld game install (see cmake/Deploy.cmake).
include(${CMAKE_SOURCE_DIR}/cmake/Deploy.cmake)
palworld_add_deploy_target(${TARGET})
