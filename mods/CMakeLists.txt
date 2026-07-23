# Each mod lives in its own subdirectory. To add a new mod, drop its folder under
# mods/ and add another add_subdirectory() line here.

# Co-locate every mod's DLL with UE4SS.dll. RE-UE4SS sets CMAKE_RUNTIME_OUTPUT_DIRECTORY
# only within its own scope, so without this mod DLLs would land in build/mods/<name>/.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/bin")

add_subdirectory(PalworldEditor)
