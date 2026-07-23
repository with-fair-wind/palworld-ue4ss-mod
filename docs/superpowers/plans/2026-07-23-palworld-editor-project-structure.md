# PalworldEditor Project Structure Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rename the current Mod to `PalworldEditor`, move every retained header into module-specific `inc/` directories, remove the static item ID list, and preserve a clean multi-Mod super-build layout.

**Architecture:** The root project remains a UE4SS super-build whose `mods/CMakeLists.txt` explicitly registers each Mod. `PalworldEditor` owns a private `inc/` include root with `game`, `skills`, and `support` modules, while implementation files stay in a flat `src/` directory. Runtime UObject scanning becomes the only source of item IDs.

**Tech Stack:** C++23, CMake 3.22+, Ninja, UE4SS, clang-format, clang-tidy, CTest

## Global Constraints

- The Mod directory, CMake target, DLL, deploy directory, UE4SS metadata, GUI labels, logs, and Hook owner use `PalworldEditor`.
- The implementation class is `PalworldEditorMod`.
- The test target is `PalworldEditorTests`; the CTest name is `PalworldEditor.SkillEditor`.
- Headers live below `mods/PalworldEditor/inc/<module>/`; `src/` remains flat.
- Mod and test include directories are private and rooted at `mods/PalworldEditor/inc`.
- `item_database.h` and its static item ID fallback are removed without replacement.
- Item IDs come only from runtime `PalStaticItemData*` UObject scanning.
- Existing historical design and plan documents retain their original names and wording.
- Do not modify `RE-UE4SS/`, `.gitignore`, `AGENTS.md`, or `UHTHeaderDump.7z`.

---

### Task 1: Rename the Mod and establish module include directories

**Files:**
- Move: `mods/MyPalMod/CMakeLists.txt` → `mods/PalworldEditor/CMakeLists.txt`
- Move: `mods/MyPalMod/src/dllmain.cpp` → `mods/PalworldEditor/src/dllmain.cpp`
- Move: `mods/MyPalMod/src/pal_skills.cpp` → `mods/PalworldEditor/src/pal_skills.cpp`
- Move: `mods/MyPalMod/src/pal_game.hpp` → `mods/PalworldEditor/inc/game/pal_game.hpp`
- Move: `mods/MyPalMod/src/pal_skills.hpp` → `mods/PalworldEditor/inc/skills/pal_skills.hpp`
- Move: `mods/MyPalMod/src/skill_catalog.hpp` → `mods/PalworldEditor/inc/skills/skill_catalog.hpp`
- Move: `mods/MyPalMod/src/skill_editor_service.hpp` → `mods/PalworldEditor/inc/skills/skill_editor_service.hpp`
- Move: `mods/MyPalMod/src/text_encoding.hpp` → `mods/PalworldEditor/inc/support/text_encoding.hpp`
- Move: `mods/MyPalMod/tests/skill_editor_tests.cpp` → `mods/PalworldEditor/tests/skill_editor_tests.cpp`
- Delete: `mods/MyPalMod/src/item_database.h`
- Modify: `mods/CMakeLists.txt`

**Interfaces:**
- Consumes: root `UE4SS` target and `palworld_add_deploy_target(<target>)`.
- Produces: CMake targets `PalworldEditor` and `PalworldEditorTests`, plus module includes rooted at `inc/`.

- [ ] **Step 1: Move the tracked Mod files and delete the static item header**

Use `apply_patch` move hunks for every retained file and a delete hunk for
`mods/MyPalMod/src/item_database.h`. Do not use a broad filesystem move because unrelated files must remain protected.

- [ ] **Step 2: Update CMake target names and include roots**

Set the Mod target to `PalworldEditor`, the test target to `PalworldEditorTests`, and both private include directories to
`${CMAKE_CURRENT_SOURCE_DIR}/inc`. Register `add_subdirectory(PalworldEditor)` from `mods/CMakeLists.txt`.

- [ ] **Step 3: Update module include paths**

Use these exact project include forms:

```cpp
#include <game/pal_game.hpp>
#include <skills/pal_skills.hpp>
#include <skills/skill_catalog.hpp>
#include <skills/skill_editor_service.hpp>
#include <support/text_encoding.hpp>
```

Remove `#include "item_database.h"`.

- [ ] **Step 4: Reconfigure and verify the renamed targets**

Run in the VS x64 developer environment:

```powershell
cmake --preset ninja-msvc-x64
ninja -C build -t targets | rg "PalworldEditor|MyPalMod"
```

Expected: `PalworldEditor` and `PalworldEditorTests` exist; no `MyPalMod` build target exists.

- [ ] **Step 5: Build the renamed targets and run tests**

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditor PalworldEditorTests
ctest --test-dir build --output-on-failure
```

Expected: both targets build; `PalworldEditor.SkillEditor` passes.

- [ ] **Step 6: Commit**

```powershell
git add mods
git commit -m "refactor: rename mod and organize public headers"
```

Stage only the renamed Mod tree and `mods/CMakeLists.txt`.

### Task 2: Remove the static item fallback and auto-scan runtime items

**Files:**
- Modify: `mods/PalworldEditor/src/dllmain.cpp`

**Interfaces:**
- Consumes: `pal_game::scan_all_items() -> std::vector<std::string>`.
- Produces: one automatic scan request after Unreal initialization and an explicit empty catalog UI state.

- [ ] **Step 1: Confirm the old fallback references are absent**

Run:

```powershell
rg -n "kBrowseItems|kBrowseItemCount|item_database" mods/PalworldEditor
```

Expected before the implementation edit: references remain only in `src/dllmain.cpp`; the deleted header is absent.

- [ ] **Step 2: Schedule the initial runtime scan**

At the end of `PalworldEditorMod::on_unreal_init()`, add:

```cpp
want_scan_items_.store(true);
```

This schedules the existing scan path for the game-thread `on_update()` callback.

- [ ] **Step 3: Render only runtime scan results**

Display `item_db_cache_.size()` directly. Iterate only over `item_db_cache_`; when it is empty, render:

```cpp
ImGui::TextDisabled("尚未发现物品，请重新扫描。");
```

Do not add another hard-coded fallback.

- [ ] **Step 4: Verify fallback removal and compile behavior**

Run:

```powershell
rg -n "kBrowseItems|kBrowseItemCount|item_database" mods/PalworldEditor
cmake --build --preset ninja-msvc-x64 --target PalworldEditor
```

Expected: ripgrep has no matches; the Mod target builds successfully.

- [ ] **Step 5: Commit**

```powershell
git add mods/PalworldEditor/src/dllmain.cpp
git commit -m "refactor: rely on runtime item discovery"
```

### Task 3: Complete naming and documentation migration

**Files:**
- Modify: `mods/PalworldEditor/src/dllmain.cpp`
- Modify: `mods/PalworldEditor/src/pal_skills.cpp`
- Modify: `mods/PalworldEditor/inc/game/pal_game.hpp`
- Modify: `mods/PalworldEditor/CMakeLists.txt`
- Modify: `cmake/Deploy.cmake`
- Modify: `README.md`
- Inspect: `scripts/build.ps1`
- Inspect: `scripts/deploy.ps1`

**Interfaces:**
- Consumes: target and runtime name `PalworldEditor`.
- Produces: consistent build, deployment, runtime, UI, and documentation naming.

- [ ] **Step 1: Replace active runtime and build identifiers**

Rename the C++ class to `PalworldEditorMod`, set `ModName`, GUI labels, window title, log prefixes, Hook owner, export macro,
target comments, and deployment examples to `PalworldEditor`. Keep the existing semantic version unless behavior versioning is
explicitly changed.

- [ ] **Step 2: Update current user documentation**

Update README build targets, artifacts, deployment paths, usage text, and directory tree to the new name and `inc/` layout.
Document that item IDs are discovered at runtime and that the first scan is automatic.

- [ ] **Step 3: Check scripts and current-source references**

Run:

```powershell
rg -n "MyPalMod" CMakeLists.txt CMakePresets.json cmake mods README.md scripts
```

Expected: no matches in active project files. Historical documents under `docs/superpowers/` and user-owned guidance files are
outside this check.

- [ ] **Step 4: Build and run tests**

```powershell
cmake --preset ninja-msvc-x64
cmake --build --preset ninja-msvc-x64 --target PalworldEditor PalworldEditorTests
ctest --test-dir build --output-on-failure
```

Expected: build succeeds and one test passes.

- [ ] **Step 5: Commit**

```powershell
git add README.md cmake/Deploy.cmake mods/PalworldEditor
git commit -m "refactor: complete PalworldEditor naming migration"
```

### Task 4: Run full project verification

**Files:**
- Verify: all task-owned files
- Do not modify: `RE-UE4SS/`, `.gitignore`, `AGENTS.md`, `UHTHeaderDump.7z`

**Interfaces:**
- Consumes: configured CMake build tree and the LLVM tools available on PATH.
- Produces: final evidence for formatting, compilation, tests, static analysis, target names, and clean task boundaries.

- [ ] **Step 1: Run format verification**

```powershell
cmake --build --preset ninja-msvc-x64 --target format-check
```

Expected: exit code 0.

- [ ] **Step 2: Run static analysis**

```powershell
cmake --build --preset ninja-msvc-x64 --target tidy-check
```

Expected: exit code 0 with existing diagnostics reported as warnings.

- [ ] **Step 3: Run final build and tests**

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditor PalworldEditorTests
ctest --test-dir build --output-on-failure
```

Expected: both targets build and `1/1` test passes.

- [ ] **Step 4: Verify names and workspace boundaries**

```powershell
rg -n "MyPalMod" CMakeLists.txt CMakePresets.json cmake mods README.md scripts
git diff --check
git diff --name-only -- RE-UE4SS
git status --short
```

Expected: no active `MyPalMod` reference, no whitespace errors, no vendored changes, and only known user-owned files remain
outside committed task changes.
