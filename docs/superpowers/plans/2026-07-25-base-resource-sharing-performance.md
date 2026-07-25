# PalworldEditor Base Resource Sharing Performance Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove the persistent and UI-frame-time overhead introduced by PalworldEditor 1.5.0 resource sharing while preserving accurate cached previews and real-time Palworld-owned consumption.

**Architecture:** Add pure value policies for Hook activation, one-second preview-cache lifetime, amount aggregation, and dirty snapshot publication. Register only four top-level resource functions while sharing is enabled; build player-inventory plus same-guild base-storage counts on cache misses, override only the two top-level preview return values, and retain the existing live transient union solely for authoritative crafting/building requests.

**Tech Stack:** C++23, CMake 3.22+, Ninja, MSVC, UE4SS experimental C++ API, PalSchema/UHT reflection, ImGui, CTest.

## Global Constraints

- Target Palworld 1.0/1.0.1 with experimental UE4SS and PalSchema.
- Support standalone and local listen-host play only.
- A disabled preference or inaccessible world must leave no resource `UFunction` Hook registered.
- Preview values may be cached for less than 1.0 second; crafting/building consumption must never use the cache.
- Preview totals include the local player's Common inventory plus every exactly resolved normal storage container owned by the same guild.
- Do not cache `UObject*`, `FProperty*`, array addresses, or item-slot addresses.
- Do not write `ItemSlotArray` or `StackCount`; Palworld remains responsible for consumption, replication, and persistence.
- Keep the 0.75-second authoritative build-union window and exact reverse restoration.
- Repair, food storage, transport, automatic production, storage UI merging, remote clients, and dedicated servers remain out of scope.
- Do not deploy to the game directory without separate authorization.

---

### Task 1: Pure Performance and Preview Policies

**Files:**
- Modify: `mods/PalworldEditor/inc/base_resource_sharing/resource_pool.hpp`
- Modify: `mods/PalworldEditor/tests/base_resource_sharing_tests.cpp`

**Interfaces:**
- Consumes: `GuidKey`, `ResourceOperation`, and the existing test harness.
- Produces:
  - `resource_hooks_required(bool enabled, bool worldAccessible) -> bool`
  - `PreviewCacheGate`
  - `SnapshotDirtyFlag`
  - `ItemAmount`
  - `AmountAggregation`
  - `aggregate_amounts(std::span<const ItemAmount>) -> AmountAggregation`
  - `max_productable_from_shared_counts(int32_t vanilla, std::span<const ItemAmount> requirements, const std::map<std::string, int64_t>& available) -> int32_t`
  - `shared_requirements_available(std::span<const ItemAmount>, const std::map<std::string, int64_t>&) -> bool`

- [ ] **Step 1: Write failing activation/cache/dirty tests**

Add:

```cpp
void test_disabled_resource_sharing_has_no_runtime_work() {
    using namespace base_resource_sharing;
    CHECK(!resource_hooks_required(false, true));
    CHECK(!resource_hooks_required(true, false));
    CHECK(resource_hooks_required(true, true));

    SnapshotDirtyFlag dirty;
    CHECK(dirty.consume());
    CHECK(!dirty.consume());
    dirty.mark();
    CHECK(dirty.consume());
}

void test_preview_cache_expires_at_one_second_and_on_world_change() {
    using namespace base_resource_sharing;
    PreviewCacheGate cache;
    cache.record(7, 10.0);
    CHECK(cache.can_reuse(7, 10.999));
    CHECK(!cache.can_reuse(7, 11.0));
    CHECK(!cache.can_reuse(8, 10.1));
    cache.invalidate();
    CHECK(!cache.can_reuse(7, 10.1));
}
```

Register both from `main()`.

- [ ] **Step 2: Run the focused test and verify RED**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorBaseResourceSharingTests
ctest --test-dir build -R PalworldEditor.BaseResourceSharing --output-on-failure
```

Expected: compilation fails because the three policy APIs do not exist.

- [ ] **Step 3: Implement the minimal policies**

Use:

```cpp
inline constexpr double kPreviewCacheSeconds = 1.0;

[[nodiscard]] constexpr auto resource_hooks_required(
    bool enabled, bool worldAccessible) noexcept -> bool {
    return enabled && worldAccessible;
}

class PreviewCacheGate {
public:
    auto record(std::uint64_t generation, double nowSeconds) noexcept -> void;
    auto invalidate() noexcept -> void;
    [[nodiscard]] auto can_reuse(
        std::uint64_t generation, double nowSeconds) const noexcept -> bool;
private:
    bool valid_{};
    std::uint64_t generation_{};
    double createdAtSeconds_{};
};

class SnapshotDirtyFlag {
public:
    auto mark() noexcept -> void;
    [[nodiscard]] auto consume() noexcept -> bool;
private:
    bool dirty_{true};
};
```

`PreviewCacheGate::can_reuse` requires a non-negative age strictly less than `1.0` and the exact generation.

- [ ] **Step 4: Run the focused test and verify GREEN**

Run the Task 1 Step 2 commands. Expected: resource-sharing CTest passes.

- [ ] **Step 5: Write failing amount aggregation tests**

Add:

```cpp
void test_preview_amounts_merge_duplicates_and_preserve_vanilla() {
    using namespace base_resource_sharing;
    const std::array raw{
        ItemAmount{"Wood", 7}, ItemAmount{"Wood", 5}, ItemAmount{"Stone", 2}};
    const auto aggregated = aggregate_amounts(raw);
    CHECK(aggregated.error.empty());
    CHECK(aggregated.amounts.at("Wood") == 12);

    const std::array requirements{
        ItemAmount{"Wood", 3}, ItemAmount{"Wood", 2}, ItemAmount{"Stone", 1}};
    CHECK(max_productable_from_shared_counts(1, requirements, aggregated.amounts) == 2);
    CHECK(shared_requirements_available(requirements, aggregated.amounts));

    const std::array invalid{ItemAmount{"Wood", -1}};
    CHECK(!aggregate_amounts(invalid).error.empty());
}
```

Register it from `main()`.

- [ ] **Step 6: Run the focused test and verify RED**

Expected: compilation fails because `ItemAmount` and aggregation functions do not exist.

- [ ] **Step 7: Implement checked amount aggregation**

Define:

```cpp
struct ItemAmount {
    std::string id;
    std::int64_t amount{};
};

struct AmountAggregation {
    std::map<std::string, std::int64_t> amounts;
    std::string error;
};
```

Reject empty IDs, negative amounts, and signed 64-bit addition overflow. Merge duplicate requirements before division. Ignore zero requirements. Return the vanilla maximum unchanged on invalid or empty requirements, and never lower the vanilla value.

- [ ] **Step 8: Run tests and commit**

Run the focused test, then:

```powershell
git add mods/PalworldEditor/inc/base_resource_sharing/resource_pool.hpp `
        mods/PalworldEditor/tests/base_resource_sharing_tests.cpp
git commit -m "perf: add resource preview cache policies"
```

---

### Task 2: On-Demand Four-Hook Lifecycle and Dirty Snapshots

**Files:**
- Modify: `mods/PalworldEditor/inc/base_resource_sharing/hook_manifest.hpp`
- Modify: `mods/PalworldEditor/src/pal_base_resources.cpp`
- Modify: `mods/PalworldEditor/tests/base_resource_sharing_tests.cpp`

**Interfaces:**
- Consumes: `resource_hooks_required`, `SnapshotDirtyFlag`, `RuntimeState`.
- Produces: four-entry Hook manifest, private `unregister_resource_hooks()`, and state-change-only snapshot publication.

- [ ] **Step 1: Write a failing manifest regression test**

Add:

```cpp
void test_hook_manifest_contains_only_top_level_preview_and_consume_paths() {
    using namespace base_resource_sharing;
    const auto hooks = palworld_1_0_1_hook_manifest();
    CHECK(hooks.size() == 4);
    CHECK(std::ranges::count(hooks, HookRole::preview, &HookSpec::role) == 2);
    CHECK(std::ranges::count(hooks, HookRole::consume, &HookSpec::role) == 2);
}
```

Register it from `main()`.

- [ ] **Step 2: Run the focused test and verify RED**

Expected: the test fails because the manifest still contains 13 paths.

- [ ] **Step 3: Reduce the manifest and synchronize Hook ownership**

Remove all auxiliary entries and the unused `HookRole::uiConsistency` enumerator. In
`PalBaseResourceBridge::Impl`:

- extract `unregister_resource_hooks()` that restores an active union, unregisters valid callback IDs, clears bindings/resolutions, and marks the snapshot dirty without changing the world generation;
- make `ensure_hooks_registered()` call it once when `resource_hooks_required(...)` is false;
- when enabled, retry unresolved paths once per second, but return immediately with no allocation when all four paths are registered for the current generation;
- call `unregister_resource_hooks()` from `on_world_begin()` after restoration and from `shutdown_hooks()`;
- invalidate the preview cache on disable and world transitions.

- [ ] **Step 4: Publish snapshots only when dirty**

Add `SnapshotDirtyFlag snapshotDirty_`. Every state-changing path calls `snapshotDirty_.mark()`. `publish_snapshot()` begins with:

```cpp
if (!snapshotDirty_.consume()) {
    return;
}
```

Remove the unconditional per-frame publication at the end of `tick()`. Mark dirty only when discovery counts/errors, capability state, restoration state, configuration, enabled state, or world state actually changes.

- [ ] **Step 5: Build and run tests**

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditor PalworldEditorBaseResourceSharingTests
ctest --test-dir build -R PalworldEditor.BaseResourceSharing --output-on-failure
git diff --check
```

- [ ] **Step 6: Commit**

```powershell
git add mods/PalworldEditor/inc/base_resource_sharing/hook_manifest.hpp `
        mods/PalworldEditor/src/pal_base_resources.cpp `
        mods/PalworldEditor/tests/base_resource_sharing_tests.cpp
git commit -m "perf: disable idle resource hooks"
```

---

### Task 3: Cached Preview Counts and Top-Level Return Overrides

**Files:**
- Modify: `mods/PalworldEditor/inc/base_resource_sharing/resource_pool.hpp`
- Modify: `mods/PalworldEditor/src/pal_base_resources.cpp`
- Modify: `mods/PalworldEditor/tests/base_resource_sharing_tests.cpp`

**Interfaces:**
- Consumes: `PreviewCacheGate`, amount helpers, existing `Discovery::liveContainers`, and the four-entry Hook manifest.
- Produces:
  - `combine_preview_sources(std::span<const ItemAmount> player, std::span<const ItemAmount> bases) -> AmountAggregation`
  - runtime-only `PreviewCountSnapshot`
  - `refresh_preview_counts()`
  - crafting/building post-hook evaluators
  - elapsed-time diagnostics

- [ ] **Step 1: Add a failing combined-source regression test**

Add and register:

```cpp
void test_preview_sources_combine_player_and_base_storage() {
    using namespace base_resource_sharing;
    const std::array player{ItemAmount{"Ingot", 4}};
    const std::array bases{ItemAmount{"Ingot", 6}};
    const auto total = combine_preview_sources(player, bases);
    const std::array recipe{ItemAmount{"Ingot", 10}};
    CHECK(total.error.empty());
    CHECK(max_productable_from_shared_counts(0, recipe, total.amounts) == 1);
}
```

- [ ] **Step 2: Run the focused test and verify RED**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorBaseResourceSharingTests
ctest --test-dir build -R PalworldEditor.BaseResourceSharing --output-on-failure
```

Expected: compilation fails because `combine_preview_sources` does not exist.

- [ ] **Step 3: Implement and verify source combination**

Implement `combine_preview_sources` by copying both spans into one `std::vector<ItemAmount>` and
calling the checked `aggregate_amounts` once, so duplicate IDs and overflow across the source
boundary follow the same rules. Run the Step 2 commands and expect PASS.

- [ ] **Step 4: Implement the runtime preview snapshot**

Inside the bridge implementation define:

```cpp
struct PreviewCountSnapshot {
    std::uint64_t generation{};
    std::map<std::string, std::int64_t> amounts;
    std::string error;
};
```

`refresh_preview_counts(worldContext)`:

1. resolve the local guild and run exact `discover_guild`;
2. resolve the player's Common container with `PalPlayerInventoryData:TryGetContainerFromInventoryType(Type=0)`;
3. read each container's reflected `ItemSlotArray`;
4. for each non-null slot, read `ItemId.StaticId` and non-negative `StackCount`;
5. aggregate player plus base `ItemAmount` values with checked addition;
6. call `combine_preview_sources(playerAmounts, baseAmounts)`;
7. store only values, record `PreviewCacheGate` with `steady_clock` seconds, and log elapsed milliseconds once.

Do not retain any object/property pointer after the function returns.

- [ ] **Step 5: Add a safe reflected required-material reader**

For `PalUIProductSettingModel:GetRequiredMaterialInfos`:

- allocate a zeroed byte buffer of exact `UFunction::GetParmsSize()`;
- find `RequiredMaterialInfos` as `FArrayProperty` and `OneUnit` as `FBoolProperty`;
- initialize the array property in the parameter buffer, set `OneUnit=true`, call `ProcessEvent`, and use `FScriptArrayHelper_InContainer`;
- require inner struct fields `StaticItemId` (`FNameProperty`) and `Num` (`FIntProperty`);
- convert each valid element to `ItemAmount`;
- destroy the initialized array property on every return path with a small RAII guard.

Any reflected-layout mismatch returns an error and preserves the vanilla preview.

- [ ] **Step 6: Override only crafting preview post-results**

In the `CalcMaxProductableNum` post-hook:

- require an `FIntProperty` return and non-null `RESULT_DECL`;
- refresh the preview snapshot only when `PreviewCacheGate` misses;
- read one-unit requirements from the model;
- calculate `max(vanilla, sharedMaximum)` and write it with the reflected return property;
- never open a transient union for preview roles.

- [ ] **Step 7: Override only building preview post-results**

In the `IsExistsMaterialForBuildObject` post-hook:

- require an `FBoolProperty` return, non-null `RESULT_DECL`, and `TheStack.Locals()`;
- if vanilla is already true, leave it unchanged;
- locate the `BuildObjectData` parameter and its four `MaterialN_Id`/`MaterialN_Count` fields by name;
- merge valid non-zero requirements, refresh/reuse the preview snapshot, and set the return true only when all requirements are available.

- [ ] **Step 8: Keep consumption live and add bounded timing**

In pre-hooks, open unions only for `HookRole::consume`. In post-hooks:

- crafting restores immediately and logs discovery/union/restore elapsed milliseconds;
- building keeps the existing 0.75-second window and logs open/restore elapsed milliseconds;
- both invalidate `PreviewCacheGate` before returning;
- repeated preview cache hits produce no log.

- [ ] **Step 9: Build and run all tests**

```powershell
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor PalworldEditorTests PalworldEditorBaseResourceSharingTests
ctest --test-dir build --output-on-failure
git diff --check
```

- [ ] **Step 10: Commit**

```powershell
git add mods/PalworldEditor/inc/base_resource_sharing/resource_pool.hpp `
        mods/PalworldEditor/src/pal_base_resources.cpp `
        mods/PalworldEditor/tests/base_resource_sharing_tests.cpp
git commit -m "perf: cache resource sharing previews"
```

---

### Task 4: Release Documentation and Verification

**Files:**
- Modify: `mods/PalworldEditor/src/dllmain.cpp`
- Modify: `README.md`
- Modify: `AGENTS.md`

**Interfaces:**
- Consumes: completed performance behavior.
- Produces: PalworldEditor 1.5.1 runtime strings and game-test handoff.

- [ ] **Step 1: Update version and documentation**

Change runtime/UI/log version strings from `1.5.0` to `1.5.1`. Document:

- disabled sharing unregisters all resource hooks;
- previews use a one-second player-plus-guild-storage value cache;
- authoritative consumption always rediscovers live containers;
- the first cache rebuild or actual consumption may have a single measurable cost, but no per-frame full scan remains;
- performance logs identify cache rebuild and live union timings.

- [ ] **Step 2: Run complete verification**

```powershell
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor PalworldEditorTests PalworldEditorBaseResourceSharingTests
ctest --test-dir build --output-on-failure
git diff --check
```

Expected: DLL links, both CTest cases pass, and diff check is silent. A third-party PatternSleuth unused-import warning is allowed; new PalworldEditor warnings are not.

- [ ] **Step 3: Inspect the final scope**

```powershell
git status --short
git diff --stat
rg -n "1\\.5\\.0|uiConsistency|StackCount.*Set|ItemSlotArray.*Set" README.md AGENTS.md mods/PalworldEditor
```

Expected: only planned files are modified; no old runtime version or prohibited item-slot/count writes remain in the resource bridge.

- [ ] **Step 4: Commit**

```powershell
git add README.md AGENTS.md mods/PalworldEditor/src/dllmain.cpp
git commit -m "docs: release PalworldEditor 1.5.1"
```

- [ ] **Step 5: Hand off game validation without deployment**

Report the built DLL at `build/Game__Shipping__Win64/bin/PalworldEditor.dll`. Do not run the deploy target. Ask the user to compare:

1. sharing disabled versus the previous build;
2. factory/build menus with sharing enabled;
3. preview refresh after one second;
4. actual remote-base crafting/building consumption;
5. LoadMap and toggle-off restoration.
