# Building Menu Resource Session Boundaries Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the first building-menu open consume the active shared-resource union immediately and make every later menu open bind to the player's current base.

**Architecture:** Treat `OnOpenMenu`/`Setup` as low-frequency building-menu boundaries, `Setup` post as the one native eligibility refresh, and `Dispose` post as the normal release boundary. Keep list/material hooks as fixed-size touches, and keep submit-time base/sequence validation as the final safety gate.

**Tech Stack:** C++23, UE4SS Experimental, Palworld 1.0.1 reflected UFunctions, CMake, Ninja, MSVC, CTest.

## Global Constraints

- Do not add EngineTick work, timers, background threads, global UObject scans, slot scans, or per-item scans.
- Do not resolve the current base or mutate Unreal arrays from high-frequency list/material hooks.
- All UObject access and `ProcessEvent` calls remain on the game thread.
- Cross-frame state contains only standard-library values, GUIDs, names, and restoration ledgers.
- A restoration failure safely disables crafting and building sharing for the current world.
- Building submit validation must still verify the current base and the applied sequence before the original request.

---

## File Structure

- `mods/PalworldEditor/inc/base_resource_sharing/hook_manifest.hpp`
  - Owns the exact menu-boundary, high-frequency, submit, and close Hook routes.
- `mods/PalworldEditor/inc/base_resource_sharing/resource_pool.hpp`
  - Owns pure decisions for reusing/replacing a building union and allowing one BuildModel refresh.
- `mods/PalworldEditor/inc/base_resource_sharing/resource_session.hpp`
  - Owns foreground operation and one-refresh-per-session state.
- `mods/PalworldEditor/src/pal_base_resources.cpp`
  - Adapts Hook events to catalog discovery, base resolution, union lifetime, refresh, and restore operations.
- `mods/PalworldEditor/tests/base_resource_sharing_tests.cpp`
  - Covers pure Hook/state/decision behavior without Unreal.
- `AGENTS.md` and `README.md`
  - Document the corrected event boundaries and performance contract.

---

### Task 1: Define the exact building-menu Hook lifecycle

**Files:**
- Modify: `mods/PalworldEditor/tests/base_resource_sharing_tests.cpp`
- Modify: `mods/PalworldEditor/inc/base_resource_sharing/hook_manifest.hpp`

**Interfaces:**
- Produces: `HookEvent::beginBuildingMenu`
- Produces: `HookEvent::closeBuilding`
- Produces: a ten-entry `kPalworld101HookManifest`

- [ ] **Step 1: Replace the existing building eligibility Hook test with the desired lifecycle**

Add assertions equivalent to:

```cpp
void test_hook_manifest_uses_low_frequency_building_menu_boundaries() {
    using namespace base_resource_sharing;

    const auto hooks = palworld_1_0_1_hook_manifest();
    const auto findHook = [&](const std::string_view path) {
        return std::ranges::find(hooks, path, &HookSpec::path);
    };

    const auto open = findHook("/Script/Pal.PalUIBuildModel:OnOpenMenu");
    CHECK(open != hooks.end());
    CHECK(event_for_phase(*open, HookPhase::pre) == HookEvent::beginBuildingMenu);

    const auto setup = findHook("/Script/Pal.PalUIInGameMainMenuBuildModel:Setup");
    CHECK(setup != hooks.end());
    CHECK(event_for_phase(*setup, HookPhase::pre) == HookEvent::beginBuildingMenu);
    CHECK(event_for_phase(*setup, HookPhase::post) == HookEvent::refreshBuilding);

    const auto dispose = findHook("/Script/Pal.PalUIInGameMainMenuBuildModel:Dispose");
    CHECK(dispose != hooks.end());
    CHECK(event_for_phase(*dispose, HookPhase::post) == HookEvent::closeBuilding);

    const auto buildList =
        findHook("/Script/Pal.PalUIBuildModel:GetBuildObjectDataArrayForUIDisplay");
    const auto eligibility =
        findHook("/Script/Pal.PalBuilderComponent:IsExistsMaterialForBuildObject");
    CHECK(event_for_phase(*buildList, HookPhase::pre) == HookEvent::touch);
    CHECK(event_for_phase(*eligibility, HookPhase::pre) == HookEvent::touch);
}
```

Update the minimal-manifest assertions to expect ten entries and registration completion only at ten.

- [ ] **Step 2: Run the focused test and verify RED**

Run:

```powershell
& cmd.exe /d /c 'call "C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Auxiliary\Build\vcvars64.bat" && cmake --build --preset ninja-msvc-x64 --target PalworldEditorBaseResourceSharingTests && ctest --test-dir build -R BaseResourceSharing --output-on-failure'
```

Expected: compilation fails because `beginBuildingMenu` and `closeBuilding` do not exist, or the assertions fail because the old Hook routes still use `acquire`.

- [ ] **Step 3: Implement the minimal Hook manifest**

In `HookEvent`, replace the generic building acquisition use with explicit values:

```cpp
enum class HookEvent : std::uint8_t {
    none,
    structureChanged,
    acquire,
    beginBuildingMenu,
    touch,
    validate,
    refreshBuilding,
    closeBuilding,
    closeCrafting,
    updateBuildingMode,
    enterBase,
    exitBase,
};
```

Use these exact building entries:

```cpp
HookSpec{ResourceOperation::building, HookEvent::beginBuildingMenu, HookEvent::none,
         HookRequirement::required, "/Script/Pal.PalUIBuildModel:OnOpenMenu"},
HookSpec{ResourceOperation::building, HookEvent::touch, HookEvent::none,
         HookRequirement::required,
         "/Script/Pal.PalUIBuildModel:GetBuildObjectDataArrayForUIDisplay"},
HookSpec{ResourceOperation::building, HookEvent::touch, HookEvent::none,
         HookRequirement::required,
         "/Script/Pal.PalBuilderComponent:IsExistsMaterialForBuildObject"},
HookSpec{ResourceOperation::building, HookEvent::beginBuildingMenu,
         HookEvent::refreshBuilding, HookRequirement::required,
         "/Script/Pal.PalUIInGameMainMenuBuildModel:Setup"},
HookSpec{ResourceOperation::building, HookEvent::validate, HookEvent::none,
         HookRequirement::required,
         "/Script/Pal.PalNetworkPlayerComponent:RequestBuild_ToServer"},
HookSpec{ResourceOperation::building, HookEvent::none, HookEvent::updateBuildingMode,
         HookRequirement::required, "/Script/Pal.PalBuilderComponent:ChangeMode"},
HookSpec{ResourceOperation::building, HookEvent::none, HookEvent::closeBuilding,
         HookRequirement::required,
         "/Script/Pal.PalUIInGameMainMenuBuildModel:Dispose"},
```

Keep the three crafting entries unchanged.

- [ ] **Step 4: Run the focused test and verify GREEN**

Run the Step 2 command.

Expected: `PalworldEditor.BaseResourceSharing` passes.

- [ ] **Step 5: Commit**

```powershell
git add mods/PalworldEditor/inc/base_resource_sharing/hook_manifest.hpp mods/PalworldEditor/tests/base_resource_sharing_tests.cpp
git commit -m "fix: define building menu resource hook boundaries"
```

---

### Task 2: Make union reuse and BuildModel refresh decisions testable

**Files:**
- Modify: `mods/PalworldEditor/tests/base_resource_sharing_tests.cpp`
- Modify: `mods/PalworldEditor/inc/base_resource_sharing/resource_pool.hpp`

**Interfaces:**
- Produces: `BuildingMenuBoundaryAction`
- Produces: `decide_building_menu_boundary(...)`
- Produces: `should_refresh_building_inventory(...)`

- [ ] **Step 1: Write failing pure-behavior tests**

Add:

```cpp
void test_building_menu_boundary_reuses_only_the_same_live_base() {
    using namespace base_resource_sharing;

    const GuidKey baseA{{7, 0, 0, 0}};
    const GuidKey baseB{{8, 0, 0, 0}};
    const auto buildingA = make_exposure_plan(ResourceOperation::building, baseA);

    CHECK(decide_building_menu_boundary(false, 7, 7, buildingA, baseA) ==
          BuildingMenuBoundaryAction::acquire);
    CHECK(decide_building_menu_boundary(true, 7, 7, buildingA, baseA) ==
          BuildingMenuBoundaryAction::reuse);
    CHECK(decide_building_menu_boundary(true, 7, 7, buildingA, baseB) ==
          BuildingMenuBoundaryAction::replace);
    CHECK(decide_building_menu_boundary(true, 6, 7, buildingA, baseA) ==
          BuildingMenuBoundaryAction::replace);
    CHECK(decide_building_menu_boundary(true, 7, 7, buildingA, std::nullopt) ==
          BuildingMenuBoundaryAction::replace);
}

void test_building_inventory_refresh_accepts_only_current_base_module_ledger() {
    using namespace base_resource_sharing;

    const GuidKey base{{7, 0, 0, 0}};
    const auto building = make_exposure_plan(ResourceOperation::building, base);
    const auto crafting = make_exposure_plan(ResourceOperation::crafting, base);

    CHECK(should_refresh_building_inventory(true, true, 7, 7, building, true, false));
    CHECK(!should_refresh_building_inventory(false, true, 7, 7, building, true, false));
    CHECK(!should_refresh_building_inventory(true, true, 6, 7, building, true, false));
    CHECK(!should_refresh_building_inventory(true, true, 7, 7, crafting, true, true));
    CHECK(!should_refresh_building_inventory(true, true, 7, 7, building, true, true));
}
```

Register both tests in `main()`.

- [ ] **Step 2: Run the focused test and verify RED**

Run the Task 1 Step 2 command.

Expected: compilation fails because both pure decision APIs are absent.

- [ ] **Step 3: Implement the minimal pure decisions**

Add to `resource_pool.hpp`:

```cpp
enum class BuildingMenuBoundaryAction : std::uint8_t { acquire, reuse, replace };

[[nodiscard]] constexpr auto decide_building_menu_boundary(
    const bool unionActive, const std::uint64_t unionGeneration,
    const std::uint64_t currentGeneration, const ResourceExposurePlan& exposure,
    const std::optional<GuidKey> observedBase) noexcept -> BuildingMenuBoundaryAction {
    if (!unionActive) {
        return BuildingMenuBoundaryAction::acquire;
    }
    const bool sameBase =
        unionGeneration == currentGeneration && observedBase.has_value() &&
        exposure.operation == ResourceOperation::building &&
        exposure.surface == ResourceConsumerSurface::currentBaseModule &&
        exposure.targetBaseId == observedBase;
    return sameBase ? BuildingMenuBoundaryAction::reuse
                    : BuildingMenuBoundaryAction::replace;
}

[[nodiscard]] constexpr auto should_refresh_building_inventory(
    const bool refreshNeeded, const bool unionActive, const std::uint64_t unionGeneration,
    const std::uint64_t currentGeneration, const ResourceExposurePlan& exposure,
    const bool hasLedgerEntry, const bool helperArray) noexcept -> bool {
    return refreshNeeded && unionGeneration == currentGeneration && hasLedgerEntry &&
           !helperArray &&
           select_building_inventory_refresh_target(unionActive, exposure) ==
               BuildingInventoryRefreshTarget::buildModel;
}
```

- [ ] **Step 4: Run the focused test and verify GREEN**

Run the Task 1 Step 2 command.

Expected: `PalworldEditor.BaseResourceSharing` passes.

- [ ] **Step 5: Commit**

```powershell
git add mods/PalworldEditor/inc/base_resource_sharing/resource_pool.hpp mods/PalworldEditor/tests/base_resource_sharing_tests.cpp
git commit -m "test: define building menu union boundary decisions"
```

---

### Task 3: Adapt runtime handlers to menu boundaries

**Files:**
- Modify: `mods/PalworldEditor/src/pal_base_resources.cpp`

**Interfaces:**
- Consumes: `HookEvent::beginBuildingMenu`
- Consumes: `HookEvent::closeBuilding`
- Consumes: `decide_building_menu_boundary(...)`
- Consumes: `should_refresh_building_inventory(...)`
- Produces: `ensure_building_menu_before_original(UObject*) -> bool`
- Produces: `handle_building_menu_closed() -> void`

- [ ] **Step 1: Add the low-frequency building boundary acquisition adapter**

Add a method beside `ensure_exposure_before_original`:

```cpp
[[nodiscard]] auto ensure_building_menu_before_original(UObject* context) -> bool {
    const auto generation = runtime_.generation();
    if (!runtime_.can_extend(ResourceOperation::building, generation)) {
        return false;
    }
    remember_world_context(context);

    if (!liveUnion_.active ||
        sessions_.active(generation) != ResourceOperation::building) {
        return ensure_exposure_before_original(context, ResourceOperation::building);
    }

    std::string error;
    const auto observedBase = detail::resolve_inside_base_id(context, catalog_, error);
    static_cast<void>(currentBase_.observe(observedBase, generation));
    const auto action = decide_building_menu_boundary(
        liveUnion_.active, liveUnion_.generation, generation, liveUnion_.exposure,
        observedBase);
    if (action == BuildingMenuBoundaryAction::reuse) {
        return true;
    }

    static_cast<void>(sessions_.release(ResourceOperation::building, generation));
    restore_or_disable("建筑菜单据点边界变化");
    if (safetyDisabled_) {
        return false;
    }
    return ensure_exposure_before_original(context, ResourceOperation::building);
}
```

The method is only called by `OnOpenMenu` and `Setup` pre-hooks.

- [ ] **Step 2: Correct the Setup post refresh gate**

Replace the contradictory `playerHelper`/`helperArray` condition in
`handle_building_menu_open_complete` with:

```cpp
const bool hasEntry = liveUnion_.entry.has_value();
const bool helperArray = hasEntry && liveUnion_.entry->helperArray;
if (!should_refresh_building_inventory(
        sessions_.building_inventory_refresh_needed(generation), liveUnion_.active,
        liveUnion_.generation, generation, liveUnion_.exposure, hasEntry, helperArray)) {
    return;
}
```

Keep the existing `notify_building_inventory_changed` call and single-completion marker.

- [ ] **Step 3: Add the normal menu-close restore adapter**

Add:

```cpp
auto handle_building_menu_closed() -> void {
    const auto generation = runtime_.generation();
    const auto transition = sessions_.release(ResourceOperation::building, generation);
    if (transition.kind == ForegroundTransitionKind::released) {
        restore_or_disable("关闭建筑菜单");
    }
}
```

- [ ] **Step 4: Route the new Hook events**

In `dispatch_hook`:

```cpp
case HookEvent::beginBuildingMenu:
    static_cast<void>(ensure_building_menu_before_original(context.Context));
    break;
case HookEvent::closeBuilding:
    handle_building_menu_closed();
    break;
```

Keep `HookEvent::acquire` for crafting initialization and keep `touch` fixed-size.

- [ ] **Step 5: Build the DLL and run the focused test**

Run:

```powershell
& cmd.exe /d /c 'call "C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Auxiliary\Build\vcvars64.bat" && cmake --build --preset ninja-msvc-x64 --target PalworldEditor PalworldEditorBaseResourceSharingTests && ctest --test-dir build -R BaseResourceSharing --output-on-failure'
```

Expected: DLL builds and the focused test passes.

- [ ] **Step 6: Commit**

```powershell
git add mods/PalworldEditor/src/pal_base_resources.cpp
git commit -m "fix: bind shared resources to building menu lifetime"
```

---

### Task 4: Align documentation with the runtime contract

**Files:**
- Modify: `AGENTS.md`
- Modify: `README.md`

**Interfaces:**
- Documents: the exact open/setup/close/high-frequency behavior implemented by Tasks 1–3.

- [ ] **Step 1: Update documentation**

State explicitly:

- `PalUIBuildModel:OnOpenMenu` and `PalUIInGameMainMenuBuildModel:Setup` pre-hooks are the only building-menu acquisition boundaries.
- `Setup` post sends one `OnUpdateInventory` notification for a `currentBaseModule` union.
- `Dispose` post restores the building union.
- High-frequency list/material hooks only touch fixed-size session state.
- A different observed base replaces the old union at the next menu boundary.

- [ ] **Step 2: Check documentation and diff**

Run:

```powershell
rg -n "OnOpenMenu|Setup|Dispose|currentBaseModule|高频" AGENTS.md README.md
git diff --check
```

Expected: the corrected contract is present and `git diff --check` exits zero.

- [ ] **Step 3: Commit**

```powershell
git add AGENTS.md README.md
git commit -m "docs: document building menu resource boundaries"
```

---

### Task 5: Full verification and deployment

**Files:**
- Verify: all modified files
- Deploy: `build/Game__Shipping__Win64/bin/PalworldEditor.dll`

- [ ] **Step 1: Run the repository verification gate**

Run:

```powershell
& cmd.exe /d /c 'call "C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Auxiliary\Build\vcvars64.bat" && cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor PalworldEditorTests PalworldEditorBaseResourceSharingTests && ctest --test-dir build --output-on-failure && git diff --check'
```

Expected: format check passes, both test suites pass, and the command exits zero.

- [ ] **Step 2: Deploy and compare hashes**

Run:

```powershell
& cmd.exe /d /c 'call "C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Auxiliary\Build\vcvars64.bat" && cmake --build --preset ninja-msvc-x64 --target deploy'
Get-FileHash build\Game__Shipping__Win64\bin\PalworldEditor.dll -Algorithm SHA256
Get-FileHash 'F:\Program Files (x86)\Steam\steamapps\common\Palworld\Pal\Binaries\Win64\ue4ss\Mods\PalworldEditor\dlls\main.dll' -Algorithm SHA256
```

Expected: deployment succeeds and both SHA256 hashes are identical.

- [ ] **Step 3: Verify final repository state**

Run:

```powershell
git status --short --branch
git log -5 --oneline
```

Expected: no uncommitted files; the branch remains ahead of its tracked remote and is not pushed unless explicitly requested.

- [ ] **Step 4: Hand off the exact game test**

Ask the user to cold-start the game and verify in this order:

1. enable sharing inside base A;
2. press B once and build without opening a furnace or reopening B;
3. close B, teleport to base B, and press B once;
4. build and cancel one object to verify real deduction/refund;
5. inspect the UE4SS log for one union open, one Setup eligibility refresh, and one restore per menu session.

