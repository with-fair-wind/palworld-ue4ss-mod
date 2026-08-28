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
    src/fishing_boost/fishing_boost_gateway.cpp
    src/fishing_boost/fishing_boost_ui.cpp
    src/revive_timer/revive_timer_gateway.cpp
    src/revive_timer/revive_timer_ui.cpp
    src/waypoint_teleport/waypoint_teleport_runtime.cpp
    src/waypoint_teleport/waypoint_teleport_ui.cpp
)

target_include_directories(${TARGET} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc)

# UE4SS supplies all headers, compile flags, and definitions transitively.
target_link_libraries(${TARGET} PRIVATE UE4SS)

# Output name = PalworldEditor (DLL: PalworldEditor.dll; UE4SS installs it as dlls/main.dll).
set_target_properties(${TARGET} PROPERTIES OUTPUT_NAME ${TARGET})

if(BUILD_TESTING)
    # 统一纯 C++ 测试 target 的包含路径、语言标准、编译选项与 CTest 注册。
    # 用法：palworld_add_unit_test(<target> <ctest-name> <source...>)
    function(palworld_add_unit_test target test_name)
        add_executable(${target} ${ARGN})
        target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc)
        target_compile_features(${target} PRIVATE cxx_std_23)
        if(MSVC)
            target_compile_options(${target} PRIVATE /utf-8 /W4 /permissive-)
        endif()
        add_test(NAME ${test_name} COMMAND ${target})
    endfunction()

    palworld_add_unit_test(PalworldEditorTests PalworldEditor.SkillEditor
        tests/skill_editor_tests.cpp)
    palworld_add_unit_test(PalworldEditorCommonTests PalworldEditor.Common
        tests/parameter_direction_tests.cpp)
    palworld_add_unit_test(PalworldEditorModLifecycleTests PalworldEditor.ModLifecycle
        tests/unload_cleanup_scheduler_tests.cpp)
    palworld_add_unit_test(PalworldEditorBaseResourceSharingTests PalworldEditor.BaseResourceSharing
        tests/base_resource_sharing_tests.cpp)
    palworld_add_unit_test(PalworldEditorRemotePalboxTests PalworldEditor.RemotePalbox
        tests/remote_palbox_tests.cpp)
    palworld_add_unit_test(PalworldEditorCaptureOverrideTests PalworldEditor.CaptureOverride
        tests/capture_override_tests.cpp)
    palworld_add_unit_test(PalworldEditorReviveTimerTests PalworldEditor.ReviveTimer
        tests/revive_timer_tests.cpp)
    palworld_add_unit_test(PalworldEditorFishingBoostTests PalworldEditor.FishingBoost
        tests/fishing_boost_tests.cpp)
    palworld_add_unit_test(PalworldEditorWaypointTeleportTests PalworldEditor.WaypointTeleport
        tests/waypoint_teleport_tests.cpp)
endif()

# One-click deploy into the Palworld game install (see cmake/Deploy.cmake).
include(${CMAKE_SOURCE_DIR}/cmake/Deploy.cmake)
palworld_add_deploy_target(${TARGET})
