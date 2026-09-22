# DeploySettings.cmake — deploy helper run via `cmake -P` at build time.
# Installs the UE4SS settings template on first deployment only: a settings file
# that already exists at the destination is user configuration and must never be
# overwritten (upstream upgrades add new keys; users merge them by hand).
#
# Usage:
#   cmake -DSETTINGS_SRC=<repo default ini> -DSETTINGS_DST=<game ini> -P DeploySettings.cmake

if(NOT DEFINED SETTINGS_SRC OR NOT DEFINED SETTINGS_DST)
    message(FATAL_ERROR "DeploySettings.cmake requires SETTINGS_SRC and SETTINGS_DST")
endif()

if(EXISTS "${SETTINGS_DST}")
    message(STATUS "settings 已存在，跳过：${SETTINGS_DST}（升级新增的配置项请手动对照 ${SETTINGS_SRC} 合并）")
else()
    file(COPY_FILE "${SETTINGS_SRC}" "${SETTINGS_DST}")
    message(STATUS "settings 已部署（首次安装）：${SETTINGS_DST}")
endif()
