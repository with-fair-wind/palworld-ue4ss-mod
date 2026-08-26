# Event-Driven Base Resource Pool Implementation Plan


> **历史方案（superseded）**：本文提到的 `resource_session` 会话机制已删除，由世界代次内
> 持续存在、可逆的公会仓储登记图取代；相关描述仅作历史记录。
> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace PalworldEditor’s per-second full resource scan and late request-only union with an event-driven, session-scoped same-guild resource pool that is visible before build/craft validation and remains safe across GC and LoadMap.

**Architecture:** Pure C++ scheduler, lease, catalog, and restoration policies remain independent of Unreal and are covered by the existing CTest target. A focused Unreal runtime adapter discovers bases and chests through Palworld manager APIs without `FindAllOf`, applies one union per active build/craft session, and restores it by recorded multiplicity. The bridge registers lightweight structural and session UFunction hooks only while sharing is enabled in an accessible local-authority world.

**Tech Stack:** C++23, CMake 3.22+, Ninja/MSVC, RE-UE4SS experimental, Unreal reflection/UFunction hooks, standard-library-only unit tests and CTest.

## Global Constraints

- Support only Palworld 1.0 single-player and local listen-server host; reject remote clients and dedicated servers.
- Perform all UObject lookup, reflection, `ProcessEvent`, array mutation, and hook callbacks on the game thread.
- Never retain raw `UObject*`, `FProperty*`, `FScriptArray` addresses, or slot addresses across callbacks.
- Do not write `ItemSlotArray`, `StackCount`, save files, or manually deduct items.
- Include only same-guild ordinary containers registered in `PalBaseCampModuleItemStorage.ContainerInfos`.
- Do not permanently broaden box UI, Pal transport, automatic production, food boxes, guild boxes, or other guild resources.
- Do not use `FindAllOf` for base modules, item containers, or multi-helpers.
- Unregister resource hooks and restore active unions before disabling, LoadMap, or unload.
- Keep repair sharing unavailable.
- Implement with TDD: every pure-policy behavior starts with a failing test and an observed RED result.
- Target release version is exactly `1.6.0`.

## File Structure

- Create `mods/PalworldEditor/inc/base_resource_sharing/resource_session.hpp`: deterministic reconcile scheduler and build/craft union lease state.
- Create `mods/PalworldEditor/src/pal_base_resource_runtime.hpp`: private Unreal adapter value types and discovery/apply/restore declarations.
- Create `mods/PalworldEditor/src/pal_base_resource_runtime.cpp`: Palworld manager traversal, local-authority gate, container resolution, union mutation, and restoration.
- Modify `mods/PalworldEditor/inc/base_resource_sharing/resource_pool.hpp`: retain catalog filtering and runtime/capability state; replace preview/request-window policies with multiplicity-based restoration policy.
- Modify `mods/PalworldEditor/inc/base_resource_sharing/hook_manifest.hpp`: describe structural, build-session, and craft-session hooks with required/optional capability semantics.
- Rewrite `mods/PalworldEditor/src/pal_base_resources.cpp`: orchestrate hooks, scheduler, leases, directory snapshots, union lifecycle, and GUI snapshot publication.
- Modify `mods/PalworldEditor/tests/base_resource_sharing_tests.cpp`: replace obsolete preview cache and 0.75-second window tests with scheduler, lease, hook-capability, and restoration regression tests.
- Modify `mods/PalworldEditor/CMakeLists.txt`: compile the new runtime adapter source.
- Modify `mods/PalworldEditor/src/dllmain.cpp`, `README.md`, `AGENTS.md`, and `CLAUDE.md`: release 1.6.0 and document the new runtime contract and manual validation.

---

### Task 1: Add Deterministic Reconcile and Union-Lease Policies

**Files:**
- Create: `mods/PalworldEditor/inc/base_resource_sharing/resource_session.hpp`
- Modify: `mods/PalworldEditor/tests/base_resource_sharing_tests.cpp`

**Interfaces:**
- Produces:
  - `inline constexpr float kCatalogRetrySeconds = 1.0F`
  - `inline constexpr float kCatalogReconcileSeconds = 8.0F`
  - `inline constexpr float kCraftingLeaseIdleSeconds = 1.5F`
  - `class ReconcileScheduler`
  - `class ResourceUnionLeaseState`
- `ReconcileScheduler` methods:
  - `begin_world(std::uint64_t generation) noexcept`
  - `request_immediate(std::uint64_t generation) noexcept`
  - `advance(float deltaSeconds, std::uint64_t generation) noexcept -> bool`
  - `complete(bool success, std::uint64_t generation) noexcept`
  - `reset() noexcept`
- `ResourceUnionLeaseState` methods:
  - `begin_world(std::uint64_t generation) noexcept`
  - `acquire_building(std::uint64_t generation) noexcept -> bool`
  - `release_building(std::uint64_t generation) noexcept -> bool`
  - `touch_crafting(std::uint64_t generation) noexcept -> bool`
  - `advance(float deltaSeconds, std::uint64_t generation) noexcept -> bool`
  - `desired(std::uint64_t generation) const noexcept -> bool`
  - `reset() noexcept`

- [ ] **Step 1: Write failing scheduler tests**

Add tests that express initial, event, fallback, retry, and generation behavior:

```cpp
void test_reconcile_scheduler_coalesces_events_and_uses_bounded_intervals() {
    using namespace base_resource_sharing;

    ReconcileScheduler scheduler;
    scheduler.begin_world(7);
    CHECK(scheduler.advance(0.0F, 7));
    scheduler.complete(true, 7);
    CHECK(!scheduler.advance(7.999F, 7));
    CHECK(scheduler.advance(0.001F, 7));
    scheduler.complete(true, 7);

    scheduler.request_immediate(7);
    scheduler.request_immediate(7);
    CHECK(scheduler.advance(0.0F, 7));
    CHECK(!scheduler.advance(0.0F, 7));
    scheduler.complete(false, 7);
    CHECK(!scheduler.advance(0.999F, 7));
    CHECK(scheduler.advance(0.001F, 7));
    scheduler.complete(true, 7);

    CHECK(!scheduler.advance(8.0F, 8));
}
```

- [ ] **Step 2: Write failing lease tests**

```cpp
void test_union_leases_overlap_and_crafting_expires_after_idle() {
    using namespace base_resource_sharing;

    ResourceUnionLeaseState leases;
    leases.begin_world(11);
    CHECK(leases.acquire_building(11));
    CHECK(leases.touch_crafting(11));
    CHECK(leases.desired(11));
    CHECK(!leases.advance(1.5F, 11));
    CHECK(leases.desired(11));
    CHECK(!leases.release_building(12));
    CHECK(leases.release_building(11));
    CHECK(!leases.desired(11));

    CHECK(leases.touch_crafting(11));
    CHECK(!leases.advance(1.499F, 11));
    CHECK(leases.advance(0.001F, 11));
    CHECK(!leases.desired(11));
    CHECK(!leases.touch_crafting(12));
}
```

- [ ] **Step 3: Run the resource tests and observe RED**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorBaseResourceSharingTests
```

Expected: compilation fails because `resource_session.hpp`, `ReconcileScheduler`, and `ResourceUnionLeaseState` do not exist.

- [ ] **Step 4: Implement the minimal deterministic policies**

Implement both classes with clamped non-negative delta time, explicit world generation checks, an in-flight reconcile flag, an 8-second success interval, a 1-second failure retry, a building boolean, and a 1.5-second crafting idle timer. `advance()` must return `true` exactly once per due reconcile or exactly once when crafting expiry changes desired-union state.

- [ ] **Step 5: Run the focused tests and observe GREEN**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorBaseResourceSharingTests
ctest --test-dir build -R PalworldEditor.BaseResourceSharing --output-on-failure
```

Expected: build and CTest pass.

- [ ] **Step 6: Commit the policy layer**

```powershell
git add mods/PalworldEditor/inc/base_resource_sharing/resource_session.hpp mods/PalworldEditor/tests/base_resource_sharing_tests.cpp
git commit -m "feat: add resource pool session policies"
```

---

### Task 2: Replace Tail Restoration with Multiplicity-Based Restoration

**Files:**
- Modify: `mods/PalworldEditor/inc/base_resource_sharing/resource_pool.hpp`
- Modify: `mods/PalworldEditor/tests/base_resource_sharing_tests.cpp`

**Interfaces:**
- Produces:
  - `struct InjectionRemovalPlan { std::vector<GuidKey> kept; bool complete{}; }`
  - `remove_recorded_injections(std::span<const GuidKey> current, std::span<const GuidKey> original, std::span<const GuidKey> injected) -> InjectionRemovalPlan`
- The obsolete exact-tail, request-window, and preview policies remain temporarily so the main DLL stays buildable until Task 5
  replaces their callers.

- [ ] **Step 1: Replace the old exact-tail tests with failing multiplicity tests**

```cpp
void test_recorded_injection_removal_preserves_runtime_native_changes() {
    using namespace base_resource_sharing;

    const GuidKey a{{1, 0, 0, 0}};
    const GuidKey b{{2, 0, 0, 0}};
    const GuidKey c{{3, 0, 0, 0}};
    const GuidKey d{{4, 0, 0, 0}};
    const std::array original{a};
    const std::array current{a, b, c, d, b};
    const std::array injected{b, c};

    const auto plan = remove_recorded_injections(current, original, injected);
    CHECK(plan.complete);
    CHECK(plan.kept == std::vector<GuidKey>({a, d, b}));

    const std::array missing{a, b};
    CHECK(!remove_recorded_injections(missing, original, injected).complete);
}
```

The first case proves exactly one `b` and one `c` are removed from the end while a same-ID native `b` remains.

- [ ] **Step 2: Run tests and observe RED**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorBaseResourceSharingTests
```

Expected: compilation fails because `InjectionRemovalPlan` and `remove_recorded_injections` do not exist.

- [ ] **Step 3: Implement reverse multiplicity removal**

Count each ID in `original` and `injected`. While traversing `current`, preserve the first `original` occurrences, remove the next
recorded `injected` occurrences, and preserve later occurrences added by the game. Set `complete=false` if the current sequence cannot
account for every recorded injection. Never erase all matching occurrences indiscriminately.

- [ ] **Step 4: Inventory obsolete policies without deleting live callers**

Run:

```powershell
rg -n "PreviewCacheGate|aggregate_amounts|combine_preview_sources|max_productable_from_shared_counts|shared_requirements_available|RequestGuard|BuildUnionWindow|verify_restoration_sequence" mods/PalworldEditor
```

Expected: the new multiplicity helper is covered by tests, while obsolete policies still have callers in the old bridge. Leave those
definitions and their existing tests in place until Task 5 removes the bridge callers and deletes them atomically.

- [ ] **Step 5: Run focused tests and observe GREEN**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorBaseResourceSharingTests
ctest --test-dir build -R PalworldEditor.BaseResourceSharing --output-on-failure
```

Expected: resource tests pass.

- [ ] **Step 6: Commit restoration policy**

```powershell
git add mods/PalworldEditor/inc/base_resource_sharing/resource_pool.hpp mods/PalworldEditor/tests/base_resource_sharing_tests.cpp
git commit -m "refactor: make resource union restoration mutation-safe"
```

---

### Task 3: Define Event and Session Hook Capabilities

**Files:**
- Modify: `mods/PalworldEditor/inc/base_resource_sharing/hook_manifest.hpp`
- Modify: `mods/PalworldEditor/tests/base_resource_sharing_tests.cpp`

**Interfaces:**
- Produces:
  - `enum class HookAction : std::uint8_t`
    - `structureChanged`
    - `buildingModeChanged`
    - `buildingTouch`
    - `craftingAcquire`
    - `craftingTouch`
  - `enum class HookRequirement : std::uint8_t { optional, required }`
  - `HookSpec { ResourceOperation operation; HookAction action; HookRequirement requirement; std::string_view path; }`
- `evaluate_capabilities()` marks crafting/building available only when every required session hook for that operation resolves; optional structure and activity hooks contribute diagnostics but do not disable the operation.

- [ ] **Step 1: Write a failing manifest contract test**

```cpp
void test_hook_manifest_separates_required_sessions_from_optional_events() {
    using namespace base_resource_sharing;

    const auto hooks = palworld_1_0_1_hook_manifest();
    CHECK(std::ranges::count(hooks, HookAction::structureChanged, &HookSpec::action) == 4);
    CHECK(std::ranges::count(hooks, HookAction::buildingModeChanged, &HookSpec::action) == 1);
    CHECK(std::ranges::count(hooks, HookAction::buildingTouch, &HookSpec::action) == 1);
    CHECK(std::ranges::count(hooks, HookAction::craftingAcquire, &HookSpec::action) == 1);
    CHECK(std::ranges::count(hooks, HookAction::craftingTouch, &HookSpec::action) == 3);
}
```

Add a second test that resolves only optional hooks and expects both capabilities unavailable, then resolves all required building hooks and expects only building available.

- [ ] **Step 2: Run tests and observe RED**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorBaseResourceSharingTests
```

Expected: compilation fails because the old `HookRole` manifest does not expose the new action and requirement fields.

- [ ] **Step 3: Implement the exact Palworld 1.0 manifest**

Use these paths:

```text
/Script/Pal.PalBaseCampModuleItemStorage:OnRep_ContainerInfos
/Script/Pal.PalBaseCampModuleItemStorage:OnAvailableConcreteModel_ServerInternal
/Script/Pal.PalBaseCampModuleItemStorage:OnNotAvailableConcreteModel_ServerInternal
/Script/Pal.PalBaseCampModel:OnRep_ModuleArray
/Script/Pal.PalBuilderComponent:ChangeMode
/Script/Pal.PalBuilderComponent:IsExistsMaterialForBuildObject
/Script/Pal.PalUIConvertItemModel:Initialize
/Script/Pal.PalUIProductSettingModel:CalcMaxProductableNum
/Script/Pal.PalUIConvertItemModel:CanStartProduction
/Script/Pal.PalUIConvertItemModel:StartProduction
```

Mark the four structure paths and `CalcMaxProductableNum` optional. Mark build mode/touch and craft acquire/`CanStartProduction`/`StartProduction` required.

- [ ] **Step 4: Run focused tests and observe GREEN**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorBaseResourceSharingTests
ctest --test-dir build -R PalworldEditor.BaseResourceSharing --output-on-failure
```

Expected: resource tests pass.

- [ ] **Step 5: Commit the manifest**

```powershell
git add mods/PalworldEditor/inc/base_resource_sharing/hook_manifest.hpp mods/PalworldEditor/tests/base_resource_sharing_tests.cpp
git commit -m "refactor: model event-driven resource hooks"
```

---

### Task 4: Add the Manager-Based Palworld Runtime Adapter

**Files:**
- Create: `mods/PalworldEditor/src/pal_base_resource_runtime.hpp`
- Create: `mods/PalworldEditor/src/pal_base_resource_runtime.cpp`
- Modify: `mods/PalworldEditor/CMakeLists.txt`

**Interfaces:**
- Consumes:
  - `GuidKey`
  - `ContainerDescriptor`
  - `ResourceUnionPlan`
  - `remove_recorded_injections`
- Produces in `base_resource_sharing::detail`:

```cpp
struct CatalogContainer {
    GuidKey containerId;
    GuidKey ownerMapObjectId;
};

struct CatalogModule {
    GuidKey baseId;
    std::wstring objectFullName;
    std::vector<CatalogContainer> containers;
};

struct ResourceCatalogSnapshot {
    std::uint64_t generation{};
    GuidKey guildId;
    ResourceUnionPlan plan;
    std::vector<CatalogModule> modules;
    std::string error;
};

struct UnionLedgerEntry {
    std::wstring objectFullName;
    bool helperArray{};
    std::vector<GuidKey> injected;
};

struct LiveUnion {
    std::uint64_t generation{};
    GuidKey guildId;
    std::vector<UnionLedgerEntry> entries;
    bool active{};
};

auto local_authority_ready(RC::Unreal::UObject* worldContext, std::string& error) -> bool;
auto discover_catalog(RC::Unreal::UObject* worldContext, std::uint64_t generation)
    -> ResourceCatalogSnapshot;
auto apply_union(RC::Unreal::UObject* worldContext,
                 const ResourceCatalogSnapshot& catalog,
                 LiveUnion& liveUnion,
                 std::string& error) -> bool;
auto restore_union(LiveUnion& liveUnion, std::string& error) -> bool;
```

- [ ] **Step 1: Add the private adapter declarations and CMake source**

Create the internal header with the exact value-only snapshot and ledger declarations above. Add `src/pal_base_resource_runtime.cpp` to `add_library(PalworldEditor SHARED ...)`.

- [ ] **Step 2: Implement property-driven reflection call helpers**

Implement local helpers that:

- call `PalUtility:IsServer` and `PalUtility:IsDedicatedServer`;
- call `PalUtility:GetBaseCampManager` and `PalUtility:GetMapObjectManager`;
- initialize/destroy reflected array output parameters using `FArrayProperty`;
- set/read `FGuid`, object, and bool parameters by property name rather than relying on guessed padding;
- reject missing functions, properties, invalid GUIDs, and mismatched generations.

All returned `UObject*` values remain local to the current function call.

- [ ] **Step 3: Implement catalog discovery without global enumeration**

Use:

```text
PalBaseCampManager:GetBaseCampIds
PalBaseCampManager:TryGetModel
PalBaseCampModel.ModuleArray
PalBaseCampModuleItemStorage.ContainerInfos
PalMapObjectManager:FindConcreteModel
PalMapObjectConcreteModelBase:GetItemContainerModule
PalMapObjectItemContainerModule:GetContainer
```

Filter models by `GetGroupIdBelongTo`, locate the storage module by `IsA(PalBaseCampModuleItemStorage)`, parse both
`OwnerMapObjectConcreteModelInstanceId` and `ContainerIdCache.ID`, and require every catalog container to resolve to a live
ordinary container. Store only IDs and module full names in the returned snapshot.

- [ ] **Step 4: Implement session union application**

For each catalog module, re-resolve its object by full name, re-read native `ContainerInfos`, append one copied source struct for each missing global container ID, and record injected IDs. Resolve the local `PalPlayerInventoryData.InventoryMultiHelper` directly and append only missing live container pointers to its `Containers` array. Never enumerate other helpers.

After a successful append, invoke `OnRep_ContainerInfos` or `OnRep_Containers` under a self-mutation guard supplied by the caller-facing bridge. If any append fails, call `restore_union` before returning false.

- [ ] **Step 5: Implement multiplicity-based restoration**

For each ledger entry in reverse order:

- re-resolve the object by full name;
- read current container GUIDs;
- call `remove_recorded_injections`;
- remove exactly the matching recorded occurrences from highest index to lowest;
- preserve unrecorded elements and ordering;
- invoke the matching original notification;
- verify every recorded injection was removed.

A missing world object counts as no longer retaining the injected data. A surviving object with incomplete removal returns false.

- [ ] **Step 6: Build the DLL and fix compile-time ABI mismatches**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditor
```

Expected: the new adapter compiles and links even though the old bridge does not call it yet.

- [ ] **Step 7: Commit the adapter**

```powershell
git add mods/PalworldEditor/src/pal_base_resource_runtime.hpp mods/PalworldEditor/src/pal_base_resource_runtime.cpp mods/PalworldEditor/CMakeLists.txt
git commit -m "feat: add manager-based base resource runtime"
```

---

### Task 5: Rewrite the Resource Bridge Around Catalog Events and Session Leases

**Files:**
- Rewrite: `mods/PalworldEditor/src/pal_base_resources.cpp`
- Modify: `mods/PalworldEditor/inc/base_resource_sharing/pal_base_resources.hpp`
- Modify: `mods/PalworldEditor/tests/base_resource_sharing_tests.cpp`

**Interfaces:**
- Consumes:
  - `ReconcileScheduler`
  - `ResourceUnionLeaseState`
  - `detail::ResourceCatalogSnapshot`
  - `detail::LiveUnion`
  - new hook actions and capability evaluation
- Preserves the public `PalBaseResourceBridge` methods used by `dllmain.cpp`.

- [ ] **Step 1: Add failing disabled/world lifecycle assertions**

Extend pure tests so `RuntimeState` and the new scheduler/lease policies prove:

```cpp
RuntimeState state;
ReconcileScheduler scheduler;
ResourceUnionLeaseState leases;

state.set_preference(true);
state.finish_world_transition(3);
scheduler.begin_world(3);
leases.begin_world(3);
CHECK(scheduler.advance(0.0F, 3));
CHECK(leases.acquire_building(3));

state.begin_world_transition(4);
scheduler.reset();
leases.reset();
CHECK(!scheduler.advance(8.0F, 4));
CHECK(!leases.desired(4));
```

- [ ] **Step 2: Run tests and observe RED if lifecycle helpers are incomplete**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorBaseResourceSharingTests
ctest --test-dir build -R PalworldEditor.BaseResourceSharing --output-on-failure
```

Expected: any missing reset/generation behavior fails before the bridge rewrite.

- [ ] **Step 3: Replace old bridge state**

Remove:

- preview count snapshots and item-slot readers;
- `PreviewCacheGate`;
- request guard and synchronous nesting owner;
- 0.75-second build window;
- request-time global discovery;
- exact-tail patches;
- return-value rewriting.

After removing their last bridge callers, also delete the obsolete `PreviewCacheGate`, item-count aggregation, `RequestGuard`,
`BuildUnionWindow`, exact-tail verification definitions, and their old unit tests from `resource_pool.hpp` and
`base_resource_sharing_tests.cpp`.

Add:

```cpp
ReconcileScheduler reconcileScheduler_;
ResourceUnionLeaseState leases_;
detail::ResourceCatalogSnapshot catalog_;
detail::LiveUnion liveUnion_;
bool selfMutation_{};
std::chrono::steady_clock::time_point nextHookAttempt_{};
```

Keep standard-library GUI snapshots under the existing mutex.

- [ ] **Step 4: Implement event-driven tick orchestration**

`tick(deltaSeconds)` must:

1. expire crafting idle state;
2. restore the union if no lease desires it;
3. consume at most one due catalog reconcile;
4. if a union is active, restore it before rediscovery;
5. call `discover_catalog`;
6. record scheduler success/failure;
7. reapply the union only if leases still desire it;
8. publish a snapshot only when dirty.

Log one elapsed-time line per reconcile/apply/restore, including generation, bases, containers, success, and cause. Do not log per frame or per preview call.

- [ ] **Step 5: Implement hook action dispatch**

Pre/post callbacks must follow this table:

| Action | Callback behavior |
|---|---|
| `structureChanged` | post: if not `selfMutation_`, call `request_immediate(generation)` |
| `buildingModeChanged` | post: call `IsInBuildingMode`; acquire/ensure while active, otherwise release/restore |
| `buildingTouch` | pre: acquire/touch building lease and call `ensure_union(context.Context)` |
| `craftingAcquire` | post: touch crafting lease and call `ensure_union(context.Context)` |
| `craftingTouch` | pre: touch crafting lease and call `ensure_union(context.Context)` |

`ensure_union` performs one synchronous manager-based catalog discovery only when no valid catalog exists. It never performs `FindAllOf` or item-slot aggregation.

- [ ] **Step 6: Implement lifecycle safety**

- `set_enabled(false)`: restore, reset leases/scheduler/catalog, unregister hooks, then publish.
- `on_world_begin`: restore before changing generation; unregister hooks; clear all pure-value state.
- `on_world_ready`: initialize scheduler and leases for the new generation; schedule immediate discovery; do not reflect until EngineTick.
- `shutdown_hooks`: restore, unregister, reset, and revoke world access.
- Hook registration retries once per second and publishes independent crafting/building capability errors.
- Optional structure hooks may be absent; the 8-second reconcile remains active.

- [ ] **Step 7: Run focused tests and build the DLL**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorBaseResourceSharingTests PalworldEditor
ctest --test-dir build -R PalworldEditor.BaseResourceSharing --output-on-failure
```

Expected: tests pass and the DLL links without references to old preview/request-window types.

- [ ] **Step 8: Verify prohibited scans and slot writes are absent**

Run:

```powershell
rg -n "FindAllOf|ItemSlotArray|StackCount|PreviewCacheGate|BuildUnionWindow|RequestGuard|resource preview cache rebuilt|live resource union preparation" mods/PalworldEditor/src/pal_base_resources.cpp mods/PalworldEditor/src/pal_base_resource_runtime.cpp
```

Expected: no `FindAllOf`, slot/count write, old cache, or old request-window symbols. `ItemSlotArray` and `StackCount` must not appear in either resource runtime source.

- [ ] **Step 9: Commit the bridge rewrite**

```powershell
git add mods/PalworldEditor/src/pal_base_resources.cpp mods/PalworldEditor/inc/base_resource_sharing/pal_base_resources.hpp mods/PalworldEditor/tests/base_resource_sharing_tests.cpp
git commit -m "refactor: use event-driven base resource sessions"
```

---

### Task 6: Release 1.6.0 and Align Repository Contracts

**Files:**
- Modify: `mods/PalworldEditor/src/dllmain.cpp`
- Modify: `README.md`
- Modify: `AGENTS.md`
- Modify: `CLAUDE.md`

**Interfaces:**
- Produces runtime/UI/log version `1.6.0`.
- Documents exact performance, role, lifecycle, and in-game verification behavior.

- [ ] **Step 1: Update exact runtime version strings**

Change:

```cpp
ModVersion = STR("1.6.0");
Output::send<LogLevel::Verbose>(STR("PalworldEditor loaded (v1.6.0)\n"));
ImGui::Begin("PalworldEditor v1.6.0", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
```

- [ ] **Step 2: Replace obsolete resource architecture documentation**

In all three repository guidance files, document:

- direct manager-based base/chest discovery;
- structural event invalidation plus 8-second fallback;
- build/craft session leases;
- no one-second preview slot scans;
- no global `FindAllOf` in the resource feature;
- multiplicity restoration and GC-safe value-only cross-frame state;
- local-authority gate;
- incompatibility with simultaneously enabled IntegratedStorage, UBIM Lite, BlueprintResearch resource hooks, or equivalent mods.

Remove 1.5.3 statements that describe one-second numeric preview caches, per-request 0.75-second build unions, or unchanged 1.5.2 resource behavior.

- [ ] **Step 3: Verify version and stale-contract cleanup**

Run:

```powershell
rg -n "1\\.5\\.3|kPreviewCacheSeconds|0\\.75|每 1 秒|每秒|FindAllOf\\(\" README.md AGENTS.md CLAUDE.md mods/PalworldEditor/src/dllmain.cpp
```

Expected: no stale runtime version or obsolete resource contract remains; historical design/plan documents are intentionally excluded.

- [ ] **Step 4: Commit the release documentation**

```powershell
git add mods/PalworldEditor/src/dllmain.cpp README.md AGENTS.md CLAUDE.md
git commit -m "docs: release PalworldEditor 1.6.0"
```

---

### Task 7: Full Verification and Deployment Artifact

**Files:**
- Verify all files changed by Tasks 1–6.
- No new implementation files unless a verification failure identifies a specific defect.

**Interfaces:**
- Produces a format-clean, test-passing `PalworldEditor.dll`.

- [ ] **Step 1: Load the verification-before-completion skill**

Read and follow `superpowers:verification-before-completion` before making any completion claim.

- [ ] **Step 2: Run required repository verification**

Run from an MSVC x64 developer environment:

```powershell
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor PalworldEditorTests PalworldEditorBaseResourceSharingTests
ctest --test-dir build --output-on-failure
git diff --check
```

Expected: every command exits 0.

- [ ] **Step 3: Inspect the final source for architectural regressions**

Run:

```powershell
rg -n "FindAllOf|ItemSlotArray|StackCount|PreviewCacheGate|BuildUnionWindow|RequestGuard" mods/PalworldEditor/src/pal_base_resources.cpp mods/PalworldEditor/src/pal_base_resource_runtime.cpp
git status --short
git diff --stat HEAD~6..HEAD
```

Expected: prohibited runtime patterns are absent and only planned files are modified.

- [ ] **Step 4: Build the deployable DLL**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditor
Get-Item build/Game__Shipping__Win64/bin/PalworldEditor.dll | Select-Object FullName,Length,LastWriteTime
```

Expected: `PalworldEditor.dll` exists with a current timestamp.

- [ ] **Step 5: Report the required game-only checks**

State explicitly that CTest cannot prove Palworld’s runtime consumption path. Hand off these isolated checks:

1. Disable IntegratedStorage and other resource-path mods.
2. Enable PalworldEditor sharing and enter a local world.
3. Verify base/container counts appear before placing a building.
4. Build with zero local and sufficient remote materials.
5. Craft with zero local and sufficient remote materials.
6. Confirm no periodic one-second log or frame-time spike.
7. Add/remove a chest and verify next-tick or at most 8-second reconciliation.
8. Exit menus, disable sharing, and LoadMap repeatedly to verify restoration and crash safety.
