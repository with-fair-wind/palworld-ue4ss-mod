# Update an existing UE4SS installation with the runtime and mod built together.
set(PALWORLD_INSTALL_DIR "$ENV{PALWORLD_INSTALL_DIR}" CACHE PATH "Palworld installation root")
set(PALWORLD_UE4SS_DIR "" CACHE PATH "Actual UE4SS working directory (optional explicit override)")

function(palworld_add_deploy_target MOD_TARGET)
    find_program(PALWORLD_PWSH NAMES pwsh REQUIRED)
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/deployment-build.json" CONTENT "{
  \"runtimeDll\": \"$<TARGET_FILE:UE4SS>\",
  \"modDll\": \"$<TARGET_FILE:${MOD_TARGET}>\"
}
")
    add_custom_target(deploy
        COMMAND "${PALWORLD_PWSH}" -NoProfile -File "${CMAKE_SOURCE_DIR}/scripts/deploy.ps1"
            -BuildDirectory "${CMAKE_BINARY_DIR}" -InstallDirectory "${PALWORLD_INSTALL_DIR}"
            -RuntimeDirectory "${PALWORLD_UE4SS_DIR}"
        DEPENDS UE4SS ${MOD_TARGET} USES_TERMINAL VERBATIM)
    # Preserve the branch's original deployment target name.
    add_custom_target(deploy-ue4ss DEPENDS deploy)
endfunction()
