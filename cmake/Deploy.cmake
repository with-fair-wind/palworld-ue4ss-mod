# Deploy.cmake — adds a `deploy` custom target that installs a built UE4SS C++ mod
# into a Palworld game installation.
#
# Usage (from a mod's CMakeLists.txt):
#   include(${CMAKE_SOURCE_DIR}/cmake/Deploy.cmake)
#   palworld_add_deploy_target(PalworldEditor)
#
# The game install path is taken from the PALWORLD_INSTALL_DIR cache variable, which
# can be set via -DPALWORLD_INSTALL_DIR=<path> or the PALWORLD_INSTALL_DIR env var.
# It should point at the "Palworld" folder containing Pal/Binaries/Win64.

set(PALWORLD_INSTALL_DIR "$ENV{PALWORLD_INSTALL_DIR}"
    CACHE PATH "Path to the Palworld game install (folder containing Pal/Binaries/Win64)")

function(palworld_add_deploy_target MOD_TARGET)
    set(_mod_name "${MOD_TARGET}")
    set(_dll_src "$<TARGET_FILE:${_mod_name}>")

    if(NOT PALWORLD_INSTALL_DIR OR PALWORLD_INSTALL_DIR STREQUAL "")
        # Create the target but make it fail loudly with guidance, so the build itself
        # (without --target deploy) is never blocked by a missing game path.
        add_custom_target(deploy
            COMMENT "deploy：${_mod_name}（未配置游戏路径）"
            COMMAND ${CMAKE_COMMAND} -E echo "错误：未设置 PALWORLD_INSTALL_DIR。"
            COMMAND ${CMAKE_COMMAND} -E echo "       请用 -DPALWORLD_INSTALL_DIR=<Palworld 路径> 重新配置"
            COMMAND ${CMAKE_COMMAND} -E echo "       （或在配置前设置 PALWORLD_INSTALL_DIR 环境变量）。"
            COMMAND ${CMAKE_COMMAND} -E echo "       路径应为包含 Pal/Binaries/Win64 的文件夹。"
            COMMAND ${CMAKE_COMMAND} -E false
            DEPENDS "${_mod_name}"
            VERBATIM
        )
        message(STATUS "deploy target 已创建但未启用：请设置 PALWORLD_INSTALL_DIR。")
        return()
    endif()

    set(_mods_root "${PALWORLD_INSTALL_DIR}/Pal/Binaries/Win64/ue4ss/Mods")
    set(_mod_dir   "${_mods_root}/${_mod_name}")
    set(_dll_dest  "${_mod_dir}/dlls/main.dll")

    add_custom_target(deploy
        COMMENT "deploy：${_mod_name} -> ${_mod_dir}"
        # 1. Ensure the destination exists.
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_mod_dir}/dlls"
        # 2. Copy the freshly built DLL as main.dll (UE4SS also accepts <ModName>.dll).
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_dll_src}" "${_dll_dest}"
        # 3. Enable the mod. enabled.txt is simplest; alternatively add a
        #    'PalworldEditor : 1' line above 'Keybinds' in ${_mods_root}/mods.txt.
        COMMAND ${CMAKE_COMMAND} -E touch "${_mod_dir}/enabled.txt"
        # 4. Report.
        COMMAND ${CMAKE_COMMAND} -E echo "已部署 ${_mod_name} -> ${_dll_dest}"
        COMMAND ${CMAKE_COMMAND} -E echo "已通过 ${_mod_dir}/enabled.txt 启用该 mod"
        DEPENDS "${_mod_name}"
        VERBATIM
    )

    message(STATUS "deploy target：${_mod_name} -> ${_mod_dir}")
endfunction()

# palworld_add_ue4ss_deploy_target() — adds a `deploy-ue4ss` custom target that
# deploys the built UE4SS runtime suite (UE4SS.dll, the dwmapi.dll proxy and the
# assets from RE-UE4SS/assets) into a Palworld game installation. Use this after
# upgrading the RE-UE4SS subproject so the runtime always matches the headers and
# static library the mod was linked against. Mod DLLs still go through the
# `deploy` target above (or Mods/<ModName>/dlls inside the same ue4ss folder).
#
# Deployed layout (relative to PALWORLD_INSTALL_DIR):
#   Pal/Binaries/Win64/dwmapi.dll            (proxy entry point, loads UE4SS)
#   Pal/Binaries/Win64/ue4ss/UE4SS.dll
#   Pal/Binaries/Win64/ue4ss/UE4SS-settings.ini  (first install only)
#   Pal/Binaries/Win64/ue4ss/LICENSE
#   Pal/Binaries/Win64/ue4ss/{CustomGameConfigs,MemberVarLayoutTemplates,
#                             VTableLayoutTemplates,UE4SS_Signatures,
#                             Default_UVTD_Configs,Mods}/
#
# This is a full-assets deployment (equivalent to the official zDEV package):
# CustomGameConfigs/Palworld.ini is required for Palworld, and the diagnostics
# console stays available for in-game verification. The official player-facing
# UE4SS_v*.zip additionally strips UE4SS_Signatures/VTableLayoutTemplates/
# MemberVarLayoutTemplates/CustomGameConfigs/MapGenBP and flips six settings
# keys to quiet release values (console off, hot reload off...) — none of that
# applies to a self-built developer install.
function(palworld_add_ue4ss_deploy_target)
    if(NOT PALWORLD_INSTALL_DIR OR PALWORLD_INSTALL_DIR STREQUAL "")
        add_custom_target(deploy-ue4ss
            COMMENT "deploy-ue4ss：UE4SS 运行时（未配置游戏路径）"
            COMMAND ${CMAKE_COMMAND} -E echo "错误：未设置 PALWORLD_INSTALL_DIR。"
            COMMAND ${CMAKE_COMMAND} -E echo "       请用 -DPALWORLD_INSTALL_DIR=<Palworld 路径> 重新配置"
            COMMAND ${CMAKE_COMMAND} -E echo "       （或在配置前设置 PALWORLD_INSTALL_DIR 环境变量）。"
            COMMAND ${CMAKE_COMMAND} -E echo "       路径应为包含 Pal/Binaries/Win64 的文件夹。"
            COMMAND ${CMAKE_COMMAND} -E false
            DEPENDS UE4SS proxy
            VERBATIM
        )
        message(STATUS "deploy-ue4ss target 已创建但未启用：请设置 PALWORLD_INSTALL_DIR。")
        return()
    endif()

    set(_win64_root "${PALWORLD_INSTALL_DIR}/Pal/Binaries/Win64")
    set(_ue4ss_root "${_win64_root}/ue4ss")
    set(_ue4ss_dll  "$<TARGET_FILE:UE4SS>")
    set(_proxy_dll  "$<TARGET_FILE_DIR:UE4SS>/dwmapi.dll")
    set(_assets_dir "${CMAKE_SOURCE_DIR}/RE-UE4SS/assets")

    add_custom_target(deploy-ue4ss
        COMMENT "deploy-ue4ss：UE4SS 运行时 -> ${_ue4ss_root}"
        # 1. Runtime: UE4SS.dll next to its assets, proxy DLL in Win64 root.
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_ue4ss_root}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_ue4ss_dll}" "${_ue4ss_root}/UE4SS.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_proxy_dll}" "${_win64_root}/dwmapi.dll"
        # 2. Settings: first-install only — an existing file is user configuration
        #    (upstream upgrades add keys; users merge them by hand), never overwritten.
        COMMAND ${CMAKE_COMMAND}
                -DSETTINGS_SRC=${_assets_dir}/UE4SS-settings.ini
                -DSETTINGS_DST=${_ue4ss_root}/UE4SS-settings.ini
                -P ${CMAKE_SOURCE_DIR}/cmake/DeploySettings.cmake
        # 3. License, matching both official release and dev packages.
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/RE-UE4SS/LICENSE"
                                                      "${_ue4ss_root}/LICENSE"
        # 4. Asset directories (signatures, layout templates, bundled mods...).
        COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${_assets_dir}/CustomGameConfigs"
                                                                "${_ue4ss_root}/CustomGameConfigs"
        COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${_assets_dir}/MemberVarLayoutTemplates"
                                                                "${_ue4ss_root}/MemberVarLayoutTemplates"
        COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${_assets_dir}/VTableLayoutTemplates"
                                                                "${_ue4ss_root}/VTableLayoutTemplates"
        COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${_assets_dir}/UE4SS_Signatures"
                                                                "${_ue4ss_root}/UE4SS_Signatures"
        COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${_assets_dir}/Default_UVTD_Configs"
                                                                "${_ue4ss_root}/Default_UVTD_Configs"
        COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${_assets_dir}/Mods"
                                                                "${_ue4ss_root}/Mods"
        # 4. Report.
        COMMAND ${CMAKE_COMMAND} -E echo "已部署 UE4SS.dll -> ${_ue4ss_root}/UE4SS.dll"
        COMMAND ${CMAKE_COMMAND} -E echo "已部署代理入口 -> ${_win64_root}/dwmapi.dll"
        COMMAND ${CMAKE_COMMAND} -E echo "已部署资源套件 -> ${_ue4ss_root}"
        DEPENDS UE4SS proxy
        VERBATIM
    )

    message(STATUS "deploy-ue4ss target：UE4SS 运行时 -> ${_ue4ss_root}")
endfunction()
