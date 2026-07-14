# Deploy.cmake — adds a `deploy` custom target that installs a built UE4SS C++ mod
# into a Palworld game installation.
#
# Usage (from a mod's CMakeLists.txt):
#   include(${CMAKE_SOURCE_DIR}/cmake/Deploy.cmake)
#   palworld_add_deploy_target(MyPalMod)
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
        #    'MyPalMod : 1' line above 'Keybinds' in ${_mods_root}/mods.txt.
        COMMAND ${CMAKE_COMMAND} -E touch "${_mod_dir}/enabled.txt"
        # 4. Report.
        COMMAND ${CMAKE_COMMAND} -E echo "已部署 ${_mod_name} -> ${_dll_dest}"
        COMMAND ${CMAKE_COMMAND} -E echo "已通过 ${_mod_dir}/enabled.txt 启用该 mod"
        DEPENDS "${_mod_name}"
        VERBATIM
    )

    message(STATUS "deploy target：${_mod_name} -> ${_mod_dir}")
endfunction()
