# Resource Sharing Correctness and Performance Implementation Plan


> **历史方案（superseded）**：本文提到的 `resource_session` 会话机制已删除，由世界代次内
> 持续存在、可逆的公会仓储登记图取代；相关描述仅作历史记录。
> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make same-guild cross-base resources available on the first original build/craft eligibility calculation, make displayed craft/build availability equal the amount Palworld can actually consume, and remove the overlapping unions and repeated work responsible for duplicate counts and frame hitches.

**Architecture:** Replace the current multi-session OR-combination with one foreground material-operation owner. Each operation exposes every unique ordinary chest through exactly one Palworld consumer surface: crafting uses the local inventory `InventoryMultiHelper`; building uses only the current base’s storage module. The Unreal adapter records one reversible sequence mutation, validates post-`OnRep` multiplicity, and restores before preemption, toggle-off, LoadMap, or unload. Catalog discovery remains event-driven and is bootstrapped before the first eligibility callback with a bounded one-shot fallback.

**Tech Stack:** C++23, CMake 3.22+, Ninja/MSVC, RE-UE4SS experimental, Palworld 1.0.1 reflection/UFunction hooks, ImGui, standard-library-only policy tests and CTest, in-game frame-time validation.

## Dependency

Implement and verify
`docs/superpowers/plans/2026-07-29-dynamic-runtime-controls-and-grapple-safety.md`
first. This plan assumes:

- resource sharing is process-local and default-off;
- no `config.ini` dependency remains;
- GUI requests are transferred atomically to EngineTick;
- disabling and LoadMap already enter the bridge through the existing game-thread lifecycle.

## Global Constraints

- Support only `IsServer && !IsDedicatedServer`: single-player and local listen-server host.
- Include only same-guild ordinary `Chest` containers registered in
  `PalBaseCampModuleItemStorage.ContainerInfos` and resolvable through
  `PalMapObjectManager:FindConcreteModel`.
- Never scan or mutate item slots, `ItemSlotArray`, `StackCount`, save files, output inventory, food boxes,
  transport, automatic production, or unrelated guild data.
- Never use `FindAllOf` for bases, storage modules, containers, or inventory helpers.
- Never retain `UObject*`, `UFunction*`, `FProperty*`, `FScriptArray`, element address, or params buffer
  across callbacks. Retain only GUIDs, object full names, counts, timestamps, and restoration sequences.
- All discovery, reflection, `ProcessEvent`, array mutation, hook registration, and restoration happen on
  the game thread.
- High-frequency recipe/list/eligibility hooks are O(1): touch the foreground session and return. They
  must not discover objects, reconcile catalogs, mutate arrays, allocate unbounded memory, or log.
- Exactly one foreground material operation owns the live union. Acquiring a different operation restores
  the old union before applying the new one.
- Exactly one Palworld consumer surface contains an injected container ID during one operation:
  crafting → player helper; building → current base storage module.
- Each injected GUID appears exactly once beyond its original multiplicity. Any duplicate, missing entry,
  unexpected reorder, or partial append triggers immediate rollback and safety-disables that operation.
- `StartProduction` post-hook must not release the crafting union. It refreshes the 1.5-second idle lease;
  EngineTick restores after the lease expires unless another crafting touch occurs.
- Catalog invalidations are coalesced. Do not reconcile while a live union is active; handle one pending
  invalidation immediately after restoration. Keep the 8-second idle reconciliation only as a fallback.
- The first `PalUIBuildModel:OnOpenMenu` and `PalUIConvertItemModel:Initialize` eligibility calculation must
  see a ready catalog and its operation’s union. A one-shot synchronous bootstrap is allowed only in these
  two acquire pre-hooks and must obey the same performance budget.
- Count all same-guild base models, including terminal-only bases with no storage module. Only bases with
  ordinary loaded chests contribute containers.
- Repair sharing remains unavailable.
- Do not coexist in validation with IntegratedStorage, UBIM Lite, BlueprintResearch, or another mod that
  changes the same material-helper/module paths.
- Implement every pure policy change with TDD and observe RED before production changes.
- C++ must follow repository formatting plus Core Guidelines: scoped enums, value types, RAII, explicit
  ownership, `std::span`, `std::optional`, structured results, const correctness, and no exceptions through
  callbacks.
- Performance release gates:
  - catalog discovery/reconciliation: `< 1.0 ms` on the five-base test save;
  - crafting helper union: `< 1.0 ms`;
  - building current-module union: `<= 16.7 ms`;
  - feature-off baseline: no repeatable measurable frame-time difference;
  - ten-minute idle: no periodic hitch or repeated log sequence.
- Correctness release gates:
  - local eggs `2` plus remote eggs `30` displays `32`, not `62`;
  - cake recipe requiring 8 eggs reports max `4` and produces `4`;
  - direct furnace first-open works without pressing B first;
  - pressing B then opening a furnace does not duplicate resources;
  - a fifth terminal-only base is counted and begins contributing after an ordinary chest is registered;
  - first building-menu open has eligible icons without opening another structure first.
- If any performance or correctness gate fails, do not bump the version or mark the plan complete.
- Preserve the unrelated user file
  `docs/superpowers/plans/2026-07-26-extensible-material-operation-sessions.md`.
- Target combined release version: `1.6.8`.

## File Structure

- **Modify:** `mods/PalworldEditor/inc/base_resource_sharing/resource_pool.hpp` — replace boolean union
  targets with a single-consumer exposure plan and exact post-apply sequence validation.
- **Modify:** `mods/PalworldEditor/inc/base_resource_sharing/resource_session.hpp` — replace three
  concurrent operation states with one foreground owner, preemption transitions, current-base GUID state,
  and the existing bounded scheduler.
- **Modify:** `mods/PalworldEditor/inc/base_resource_sharing/hook_manifest.hpp` — add current-base
  enter/exit events and stop releasing crafting in `StartProduction` post.
- **Modify:** `mods/PalworldEditor/inc/base_resource_sharing/pal_base_resources.hpp` — update snapshots and
  bridge surface only where new status/count values are required.
- **Modify:** `mods/PalworldEditor/src/pal_base_resource_runtime.hpp` — replace `UnionTargets` with
  `ResourceExposurePlan`, add current-base resolution and exact sequence-validation declarations.
- **Modify:** `mods/PalworldEditor/src/pal_base_resource_runtime.cpp` — count terminal-only bases, resolve
  current base, mutate one consumer surface, validate multiplicity, and rollback safely.
- **Modify:** `mods/PalworldEditor/src/pal_base_resources.cpp` — implement single-owner preemption,
  first-eligibility bootstrap, event coalescing, idle release, and O(1) touch hooks.
- **Modify:** `mods/PalworldEditor/src/dllmain.cpp` — show precise capability/status and keep lifecycle
  ordering; do not add resource work to ImGui or empty `on_update`.
- **Modify:** `mods/PalworldEditor/tests/base_resource_sharing_tests.cpp` — replace combined-target and
  concurrent-session tests with exposure, preemption, multiplicity, current-base, manifest, and scheduler
  regressions.
- **Modify:** `README.md`, `AGENTS.md`, `CLAUDE.md` — document the corrected single-surface model,
  performance contract, manual validation, and version `1.6.8`.

---

## Task 1: Replace Combined Targets with a Single-Consumer Exposure Plan

**Files:**

- Modify: `mods/PalworldEditor/inc/base_resource_sharing/resource_pool.hpp`
- Modify: `mods/PalworldEditor/tests/base_resource_sharing_tests.cpp`

**Interfaces:**

Remove:

```cpp
struct UnionTargets;
combine_union_targets(...);
contains_union_targets(...);
union_targets_for_operation(...);
```

Add:

```cpp
enum class ResourceConsumerSurface : std::uint8_t {
    none,
    playerHelper,
    currentBaseModule,
};

struct ResourceExposurePlan {
    ResourceOperation operation{ResourceOperation::repair};
    ResourceConsumerSurface surface{ResourceConsumerSurface::none};
    std::optional<GuidKey> targetBaseId;

    auto operator<=>(const ResourceExposurePlan&) const = default;
};

[[nodiscard]] auto make_exposure_plan(
    ResourceOperation operation,
    std::optional<GuidKey> currentBaseId = std::nullopt) noexcept
    -> ResourceExposurePlan;
```

Rules:

- crafting → `{crafting, playerHelper, nullopt}`;
- building with a valid base → `{building, currentBaseModule, baseId}`;
- building without a valid base → `surface == none`;
- repair → `surface == none`.

- [ ] **Step 1: Write failing exposure-policy tests**

Replace the old combined-target test with:

```cpp
void test_resource_exposure_uses_exactly_one_consumer_surface() {
    using namespace base_resource_sharing;

    const GuidKey base{{10, 0, 0, 0}};
    const auto crafting = make_exposure_plan(ResourceOperation::crafting);
    CHECK(crafting.surface == ResourceConsumerSurface::playerHelper);
    CHECK(!crafting.targetBaseId.has_value());

    const auto building = make_exposure_plan(ResourceOperation::building, base);
    CHECK(building.surface == ResourceConsumerSurface::currentBaseModule);
    CHECK(building.targetBaseId == base);

    CHECK(make_exposure_plan(ResourceOperation::building).surface ==
          ResourceConsumerSurface::none);
    CHECK(make_exposure_plan(ResourceOperation::repair).surface ==
          ResourceConsumerSurface::none);
}
```

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorBaseResourceSharingTests
```

Expected: RED because `ResourceConsumerSurface` and `make_exposure_plan` do not exist.

- [ ] **Step 2: Implement the exposure policy**

Use a `switch` with no boolean combinations:

```cpp
[[nodiscard]] inline auto make_exposure_plan(
    const ResourceOperation operation,
    const std::optional<GuidKey> currentBaseId) noexcept -> ResourceExposurePlan {
    switch (operation) {
    case ResourceOperation::crafting:
        return {.operation = operation,
                .surface = ResourceConsumerSurface::playerHelper};
    case ResourceOperation::building:
        if (currentBaseId.has_value() && currentBaseId->valid()) {
            return {.operation = operation,
                    .surface = ResourceConsumerSurface::currentBaseModule,
                    .targetBaseId = currentBaseId};
        }
        return {.operation = operation};
    case ResourceOperation::repair:
        return {.operation = operation};
    }
    return {.operation = operation};
}
```

Do not preserve compatibility overloads that can recreate module-plus-helper combinations.

- [ ] **Step 3: Add exact sequence validation**

Add:

```cpp
enum class SequenceValidationStatus : std::uint8_t {
    valid,
    originalPrefixChanged,
    injectedCountMismatch,
    duplicateInjectedId,
    unexpectedTail,
};

struct SequenceValidationResult {
    SequenceValidationStatus status{SequenceValidationStatus::valid};
    std::optional<GuidKey> offendingId;

    [[nodiscard]] explicit operator bool() const noexcept {
        return status == SequenceValidationStatus::valid;
    }
};

[[nodiscard]] auto validate_applied_sequence(
    std::span<const GuidKey> original,
    std::span<const GuidKey> injected,
    std::span<const GuidKey> current) noexcept -> SequenceValidationResult;
```

The valid sequence is exactly `original` followed by every injected ID once, in recorded order. Original
duplicates are allowed because they predate the mod; injected IDs may not duplicate any existing or injected
ID.

- [ ] **Step 4: Write RED tests for the cake duplication failure**

```cpp
void test_applied_sequence_rejects_duplicate_remote_container() {
    using namespace base_resource_sharing;

    const GuidKey local{{1, 0, 0, 0}};
    const GuidKey remote{{2, 0, 0, 0}};
    const std::array original{local};
    const std::array injected{remote};
    const std::array valid{local, remote};
    const std::array doubled{local, remote, remote};

    CHECK(validate_applied_sequence(original, injected, valid));
    CHECK(!validate_applied_sequence(original, injected, doubled));
}

void test_applied_sequence_rejects_injection_of_existing_id() {
    using namespace base_resource_sharing;

    const GuidKey local{{1, 0, 0, 0}};
    const std::array original{local};
    const std::array injected{local};
    const std::array current{local, local};
    CHECK(!validate_applied_sequence(original, injected, current));
}
```

Run and observe RED, then implement with bounded `std::ranges`/`std::set` or sorted value copies. This runs
only when opening a union, not in high-frequency hooks.

- [ ] **Step 5: Run focused tests and commit**

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorBaseResourceSharingTests
ctest --test-dir build -R PalworldEditorBaseResourceSharingTests --output-on-failure
git diff --check
git add mods/PalworldEditor/inc/base_resource_sharing/resource_pool.hpp `
  mods/PalworldEditor/tests/base_resource_sharing_tests.cpp
git commit -m "refactor: model one resource consumer surface"
```

---

## Task 2: Replace Concurrent Sessions with One Foreground Owner

**Files:**

- Modify: `mods/PalworldEditor/inc/base_resource_sharing/resource_session.hpp`
- Modify: `mods/PalworldEditor/tests/base_resource_sharing_tests.cpp`

**Interfaces:**

Remove `MaterialOperationSessions`, `OperationState[3]`, and `required_targets`.

Add:

```cpp
enum class ForegroundTransitionKind : std::uint8_t {
    none,
    acquired,
    refreshed,
    preempted,
    released,
};

struct ForegroundTransition {
    ForegroundTransitionKind kind{ForegroundTransitionKind::none};
    std::optional<ResourceOperation> previous;
    std::optional<ResourceOperation> current;
};

class ForegroundMaterialSession {
public:
    auto begin_world(std::uint64_t generation) noexcept -> void;
    [[nodiscard]] auto acquire(ResourceOperation operation,
                               std::uint64_t generation) noexcept -> ForegroundTransition;
    [[nodiscard]] auto touch(ResourceOperation operation,
                             std::uint64_t generation) noexcept -> bool;
    [[nodiscard]] auto release(ResourceOperation operation,
                               std::uint64_t generation) noexcept -> ForegroundTransition;
    [[nodiscard]] auto advance(float deltaSeconds,
                               std::uint64_t generation) noexcept -> ForegroundTransition;
    [[nodiscard]] auto active(std::uint64_t generation) const noexcept
        -> std::optional<ResourceOperation>;
    auto reset() noexcept -> void;
};
```

Keep crafting `idleTimeout = 1.5F`; building remains explicit release on leaving building mode.

Add a pure GUID tracker:

```cpp
class CurrentBaseState {
public:
    auto begin_world(std::uint64_t generation) noexcept -> void;
    [[nodiscard]] auto enter(GuidKey baseId, std::uint64_t generation) noexcept -> bool;
    [[nodiscard]] auto exit(GuidKey baseId, std::uint64_t generation) noexcept -> bool;
    [[nodiscard]] auto current(std::uint64_t generation) const noexcept
        -> std::optional<GuidKey>;
    auto reset() noexcept -> void;
};
```

- [ ] **Step 1: Write failing preemption tests**

```cpp
void test_foreground_session_preempts_instead_of_combining_operations() {
    using namespace base_resource_sharing;

    ForegroundMaterialSession sessions;
    sessions.begin_world(7);
    const auto crafting = sessions.acquire(ResourceOperation::crafting, 7);
    CHECK(crafting.kind == ForegroundTransitionKind::acquired);
    CHECK(sessions.active(7) == ResourceOperation::crafting);

    const auto building = sessions.acquire(ResourceOperation::building, 7);
    CHECK(building.kind == ForegroundTransitionKind::preempted);
    CHECK(building.previous == ResourceOperation::crafting);
    CHECK(building.current == ResourceOperation::building);
    CHECK(sessions.active(7) == ResourceOperation::building);
}
```

Add:

```cpp
void test_foreground_session_ignores_stale_touch_and_release() {
    using namespace base_resource_sharing;

    ForegroundMaterialSession sessions;
    sessions.begin_world(7);
    static_cast<void>(sessions.acquire(ResourceOperation::building, 7));
    CHECK(!sessions.touch(ResourceOperation::crafting, 7));
    CHECK(sessions.release(ResourceOperation::crafting, 7).kind ==
          ForegroundTransitionKind::none);
    CHECK(!sessions.active(8).has_value());
}
```

Run and observe RED.

- [ ] **Step 2: Implement deterministic single-owner transitions**

Store:

```cpp
std::uint64_t generation_{};
std::optional<ResourceOperation> active_;
float idleSeconds_{};
```

`acquire` behavior:

- stale generation or repair → `none`;
- no owner → `acquired`;
- same owner → reset idle and `refreshed`;
- different owner → replace owner, reset idle, return `preempted` with both values.

`advance` expires only an active crafting owner after 1.5 seconds and returns `released`.

- [ ] **Step 3: Write and implement current-base GUID tests**

```cpp
void test_current_base_state_never_leaks_across_worlds() {
    using namespace base_resource_sharing;

    CurrentBaseState state;
    const GuidKey baseA{{10, 0, 0, 0}};
    const GuidKey baseB{{20, 0, 0, 0}};
    state.begin_world(7);
    CHECK(state.enter(baseA, 7));
    CHECK(state.current(7) == baseA);
    CHECK(!state.exit(baseB, 7));
    CHECK(state.current(7) == baseA);
    CHECK(state.exit(baseA, 7));
    CHECK(!state.current(7).has_value());
    state.begin_world(8);
    CHECK(!state.current(8).has_value());
}
```

Implement using only `GuidKey` and generation. Do not add an object pointer.

- [ ] **Step 4: Preserve scheduler behavior**

Keep existing tests for:

- initial immediate reconciliation;
- one-second retry after failure;
- eight-second idle fallback;
- coalesced invalidations;
- no advance while an operation is active.

Change the scheduler call site later to pass `mayRun = !sessions.active(generation).has_value()`.

- [ ] **Step 5: Run tests and commit**

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorBaseResourceSharingTests
ctest --test-dir build -R PalworldEditorBaseResourceSharingTests --output-on-failure
git diff --check
git add mods/PalworldEditor/inc/base_resource_sharing/resource_session.hpp `
  mods/PalworldEditor/tests/base_resource_sharing_tests.cpp
git commit -m "refactor: serialize foreground material sessions"
```

---

## Task 3: Track and Resolve the Current Base Without Background Scans

**Files:**

- Modify: `mods/PalworldEditor/inc/base_resource_sharing/hook_manifest.hpp`
- Modify: `mods/PalworldEditor/src/pal_base_resource_runtime.hpp`
- Modify: `mods/PalworldEditor/src/pal_base_resource_runtime.cpp`
- Modify: `mods/PalworldEditor/tests/base_resource_sharing_tests.cpp`

**Interfaces:**

Extend `HookEvent` with:

```cpp
enterBase,
exitBase,
```

Add exact Palworld 1.0.1 paths:

```cpp
HookSpec{ResourceOperation::building, HookEvent::enterBase, HookEvent::none,
         HookRequirement::required,
         "/Script/Pal.PalBuilderComponent:OnEnterBaseCamp"},
HookSpec{ResourceOperation::building, HookEvent::exitBase, HookEvent::none,
         HookRequirement::required,
         "/Script/Pal.PalBuilderComponent:OnExitBaseCamp"},
```

Add runtime declarations:

```cpp
[[nodiscard]] auto read_base_id(RC::Unreal::UObject* baseModel)
    -> std::optional<GuidKey>;
[[nodiscard]] auto resolve_nearest_base_id(
    RC::Unreal::UObject* worldContext,
    const ResourceCatalogSnapshot& catalog,
    std::string& error) -> std::optional<GuidKey>;
```

The fallback resolution chain is exact and one-shot:

1. `PalUtility:GetLocalPalPlayerController`;
2. controller `GetPawn`;
3. pawn `K2_GetActorLocation`;
4. `PalUtility:GetBaseCampManager`;
5. manager `GetNearestBaseCamp(FVector)`;
6. read `BaseCampId`;
7. require the ID to exist in the current same-guild catalog.

If any function/property is absent on Palworld 1.0.1, building capability is unavailable with an explicit
error. Do not substitute a global scan.

- [ ] **Step 1: Write failing manifest tests**

```cpp
void test_hook_manifest_tracks_current_base_context() {
    using namespace base_resource_sharing;

    const auto hooks = palworld_1_0_1_hook_manifest();
    const auto enter = std::ranges::find(
        hooks, std::string_view{"/Script/Pal.PalBuilderComponent:OnEnterBaseCamp"},
        &HookSpec::path);
    const auto exit = std::ranges::find(
        hooks, std::string_view{"/Script/Pal.PalBuilderComponent:OnExitBaseCamp"},
        &HookSpec::path);
    CHECK(enter != hooks.end());
    CHECK(exit != hooks.end());
    CHECK(event_for_phase(*enter, HookPhase::pre) == HookEvent::enterBase);
    CHECK(event_for_phase(*exit, HookPhase::pre) == HookEvent::exitBase);
}
```

Run and observe RED.

- [ ] **Step 2: Add manifest entries and capability requirements**

Mark both hooks required for building correctness. Missing either must disable building only, not crafting.
Update `evaluate_capabilities` tests accordingly.

- [ ] **Step 3: Read the hook parameter safely**

In `pal_base_resources.cpp`, inspect the callback function’s parameter properties to locate the object
parameter named `BaseCampModel`. From that callback-local object:

- call `detail::read_base_id`;
- immediately store only `GuidKey` through `CurrentBaseState`;
- discard the pointer before returning.

On exit, clear only when the exiting GUID equals the tracked GUID.

- [ ] **Step 4: Implement the one-shot fallback**

Use initialized/destroyed UFunction parameter storage rather than assuming a packed C++ parameter struct.
Find return properties through `CPF_ReturnParm`; validate that the location return is an `FStructProperty`
compatible with `FVector` and the base return is an object property.

Call the fallback only:

- in the building `OnOpenMenu` acquire pre-hook;
- when `CurrentBaseState::current(generation)` is empty;
- at most once for that acquire callback.

Store only the returned `GuidKey`. Do not retain controller, pawn, manager, or model.

- [ ] **Step 5: Count terminal-only bases**

In `discover_catalog`, increment a new `sameGuildBaseCount` immediately after a base model’s guild is
validated, before searching `ModuleArray` for a storage module:

```cpp
++result.sameGuildBaseCount;
```

A missing storage module is not an error and contributes zero containers. A missing or malformed
`ModuleArray` remains a structural error.

Publish `sameGuildBaseCount`, not `plan.baseCount`, as the GUI base count.

- [ ] **Step 6: Verify and commit**

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditor `
  PalworldEditorBaseResourceSharingTests
ctest --test-dir build -R PalworldEditorBaseResourceSharingTests --output-on-failure
git diff --check
git add mods/PalworldEditor/inc/base_resource_sharing/hook_manifest.hpp `
  mods/PalworldEditor/src/pal_base_resource_runtime.hpp `
  mods/PalworldEditor/src/pal_base_resource_runtime.cpp `
  mods/PalworldEditor/src/pal_base_resources.cpp `
  mods/PalworldEditor/tests/base_resource_sharing_tests.cpp
git commit -m "feat: resolve the active base for building resources"
```

---

## Task 4: Apply and Restore Exactly One Consumer Surface

**Files:**

- Modify: `mods/PalworldEditor/src/pal_base_resource_runtime.hpp`
- Modify: `mods/PalworldEditor/src/pal_base_resource_runtime.cpp`
- Modify: `mods/PalworldEditor/inc/base_resource_sharing/resource_pool.hpp`
- Modify: `mods/PalworldEditor/tests/base_resource_sharing_tests.cpp`

**Interfaces:**

Change:

```cpp
[[nodiscard]] auto apply_union(
    RC::Unreal::UObject* worldContext,
    const ResourceCatalogSnapshot& catalog,
    const ResourceExposurePlan& exposure,
    LiveUnion& liveUnion,
    std::string& error) -> bool;
```

`LiveUnion` stores:

```cpp
std::uint64_t generation{};
GuidKey guildId;
ResourceExposurePlan exposure;
std::optional<UnionLedgerEntry> entry;
bool active{};
```

There is one ledger entry because one operation mutates one consumer surface.

- [ ] **Step 1: Remove the all-modules loop**

For `currentBaseModule`:

1. require `targetBaseId`;
2. find exactly one `CatalogModule` with that base ID;
3. resolve its object by `objectFullName`;
4. read its original `ContainerInfos` sequence;
5. append every missing unique catalog container ID;
6. call `OnRep_ContainerInfos` once if anything was injected;
7. read the current sequence again;
8. validate with `validate_applied_sequence`.

Do not mutate the other four base modules.

- [ ] **Step 2: Keep crafting helper-only**

For `playerHelper`:

1. resolve `GetLocalInventoryData` during this callback;
2. resolve `InventoryMultiHelper`;
3. read the original `Containers` object sequence as GUIDs;
4. append each missing unique catalog container exactly once;
5. call the existing helper notification once;
6. read back and validate.

Do not modify any base module during crafting.

- [ ] **Step 3: Roll back on any invalid sequence**

If re-read validation fails:

- call `restore_union` before returning;
- verify restoration using the existing original-sequence-plus-runtime-additions policy;
- return an error naming the `SequenceValidationStatus`;
- do not leave `liveUnion.active == true`.

If rollback verification fails, let the bridge safety-disable both crafting and building for that world.

- [ ] **Step 4: Keep restoration lossless**

Restoration must remove exactly the recorded injected occurrences while preserving:

- every original element and original order;
- any non-injected runtime element added while the union was active;
- original duplicate multiplicity;
- elements added by Palworld after the recorded tail.

Retain the existing occurrence-ledger tests and add a single-entry `LiveUnion` regression.

- [ ] **Step 5: Add exposure invariants**

Before mutation:

```cpp
if (exposure.surface == ResourceConsumerSurface::none ||
    exposure.operation == ResourceOperation::repair) {
    error = "当前材料操作没有可用的单一消费入口。";
    return false;
}
```

After mutation:

```cpp
if (!liveUnion.entry.has_value() || liveUnion.entry->injected.empty()) {
    // A valid no-op is active only when all global IDs were already present on this one surface.
}
```

The no-op case still records the original sequence so restoration and preemption stay deterministic.

- [ ] **Step 6: Build, test, and commit**

```powershell
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor `
  PalworldEditorBaseResourceSharingTests
ctest --test-dir build -R PalworldEditorBaseResourceSharingTests --output-on-failure
git diff --check
git add mods/PalworldEditor/src/pal_base_resource_runtime.hpp `
  mods/PalworldEditor/src/pal_base_resource_runtime.cpp `
  mods/PalworldEditor/inc/base_resource_sharing/resource_pool.hpp `
  mods/PalworldEditor/tests/base_resource_sharing_tests.cpp
git commit -m "fix: expose shared containers through one consumer"
```

---

## Task 5: Preempt Old Operations and Keep Crafting Alive Through Submission

**Files:**

- Modify: `mods/PalworldEditor/inc/base_resource_sharing/hook_manifest.hpp`
- Modify: `mods/PalworldEditor/src/pal_base_resources.cpp`
- Modify: `mods/PalworldEditor/tests/base_resource_sharing_tests.cpp`

- [ ] **Step 1: Change `StartProduction` post from release to touch**

Update the manifest:

```cpp
HookSpec{ResourceOperation::crafting, HookEvent::touch, HookEvent::touch,
         HookRequirement::required,
         "/Script/Pal.PalUIConvertItemModel:StartProduction"},
```

Update the test:

```cpp
CHECK(event_for_phase(*startProduction, HookPhase::pre) == HookEvent::touch);
CHECK(event_for_phase(*startProduction, HookPhase::post) == HookEvent::touch);
```

Run and observe RED before editing the manifest.

- [ ] **Step 2: Centralize foreground acquisition**

Replace `handle_acquire` with a transition-driven method:

```cpp
auto acquire_foreground(UObject* context, ResourceOperation operation) -> void;
```

Behavior:

1. reject unavailable capability or generation;
2. get current base GUID for building;
3. create the exact `ResourceExposurePlan`;
4. call `sessions_.acquire`;
5. when transition is `preempted`, restore the old live union first;
6. if restoration fails, cancel the session and safety-disable;
7. ensure the catalog is ready;
8. apply the new exposure;
9. if apply fails, release the new owner and publish the error.

The union must never contain the previous and current operation simultaneously.

- [ ] **Step 3: Make touch hooks O(1)**

For `HookEvent::touch`:

```cpp
static_cast<void>(sessions_.touch(spec.operation, runtime_.generation()));
```

No object lookup, union call, scheduler call, log, or allocation is permitted in the touch branch.

If an acquire hook failed, later touch hooks must not try to repair it. The next explicit acquire event is
the retry boundary.

- [ ] **Step 4: Release on deterministic boundaries**

- crafting: `sessions_.advance(deltaSeconds, generation)` releases after 1.5 seconds without a touch;
- building: `PalBuilderComponent:ChangeMode` releases when `IsInBuildingMode == false`;
- operation preemption: restore old, then apply new;
- toggle off: restore, reset sessions, unregister hooks;
- LoadMap pre: restore, reset sessions/current base/scheduler, unregister hooks;
- unload preparation: same as toggle off; destructor performs no Unreal access.

- [ ] **Step 5: Pause reconciliation while active**

EngineTick passes:

```cpp
const bool idle = !sessions_.active(generation).has_value() && !liveUnion_.active;
```

to `scheduler_.advance`. Structure hooks only call:

```cpp
scheduler_.request_immediate(generation);
```

If invalidation arrives during an active union, leave it pending. Immediately after successful restoration,
allow one reconciliation; do not run one per invalidation.

- [ ] **Step 6: Run focused tests and commit**

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditor `
  PalworldEditorBaseResourceSharingTests
ctest --test-dir build -R PalworldEditorBaseResourceSharingTests --output-on-failure
git diff --check
git add mods/PalworldEditor/inc/base_resource_sharing/hook_manifest.hpp `
  mods/PalworldEditor/src/pal_base_resources.cpp `
  mods/PalworldEditor/tests/base_resource_sharing_tests.cpp
git commit -m "fix: preempt material operations without double counting"
```

---

## Task 6: Make the First Eligibility Calculation See a Ready Union

**Files:**

- Modify: `mods/PalworldEditor/src/pal_base_resources.cpp`
- Modify: `mods/PalworldEditor/inc/base_resource_sharing/resource_session.hpp`
- Modify: `mods/PalworldEditor/tests/base_resource_sharing_tests.cpp`

**Interfaces:**

Add private bridge methods:

```cpp
[[nodiscard]] auto bootstrap_catalog_for_acquire(
    RC::Unreal::UObject* worldContext,
    ResourceOperation operation) -> bool;
[[nodiscard]] auto ensure_exposure_before_original(
    RC::Unreal::UObject* worldContext,
    ResourceOperation operation) -> bool;
```

- [ ] **Step 1: Keep world-ready initialization eager but bounded**

When the world becomes accessible and the feature is desired:

- register the resource hooks;
- begin runtime/session/current-base/scheduler generation;
- request one immediate catalog reconciliation;
- on each EngineTick, run it only when local authority and required managers are ready;
- after success, rely on invalidations plus the eight-second idle fallback.

Do not wait for B or furnace interaction to begin discovery.

- [ ] **Step 2: Add a one-shot acquire fallback**

In only these required pre-hooks:

- `/Script/Pal.PalUIBuildModel:OnOpenMenu`;
- `/Script/Pal.PalUIConvertItemModel:Initialize`;

if the catalog is not ready, call `bootstrap_catalog_for_acquire(context, operation)` once. It:

1. verifies feature/world/generation/capability;
2. calls `discover_catalog` once;
3. rejects if discovery fails;
4. publishes the catalog;
5. applies the exact exposure before returning to Palworld’s original function.

Do not queue a cross-frame object pointer or attempt again from high-frequency hooks.

- [ ] **Step 3: Enforce the bootstrap budget**

Measure with `steady_clock` around catalog bootstrap and exposure application. Emit one Verbose line only
for the completed acquire:

```text
PalworldEditor: material acquire operation=crafting catalog_ms=... union_ms=... entries=1
```

If catalog bootstrap is `>= 1.0 ms`, keep correctness but fail the release performance gate and profile the
discovery path before continuing. Do not hide it by raising the limit.

- [ ] **Step 4: Eliminate stale world-context effects**

The saved world-context full name may be used only to re-resolve a context during EngineTick. On LoadMap pre:

- clear the name;
- restore first;
- reset catalog/current base/session/scheduler;
- increment generation.

An acquire callback always prefers its callback-local context and never reuses an old pointer.

- [ ] **Step 5: Add scheduler/bootstrap policy tests**

Pure tests must prove:

- initial world begin has one pending reconcile;
- repeated structure invalidation coalesces;
- active foreground session suppresses advance;
- restoration followed by idle allows exactly one pending reconcile;
- stale generation cannot bootstrap/acquire through pure transition helpers.

- [ ] **Step 6: Verify and commit**

```powershell
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor `
  PalworldEditorBaseResourceSharingTests
ctest --test-dir build -R PalworldEditorBaseResourceSharingTests --output-on-failure
git diff --check
git add mods/PalworldEditor/src/pal_base_resources.cpp `
  mods/PalworldEditor/inc/base_resource_sharing/resource_session.hpp `
  mods/PalworldEditor/tests/base_resource_sharing_tests.cpp
git commit -m "fix: prepare shared resources before eligibility checks"
```

---

## Task 7: Publish Precise Status Without Adding Per-Frame Work

**Files:**

- Modify: `mods/PalworldEditor/inc/base_resource_sharing/pal_base_resources.hpp`
- Modify: `mods/PalworldEditor/src/pal_base_resources.cpp`
- Modify: `mods/PalworldEditor/src/dllmain.cpp`

**Interfaces:**

Extend `BaseResourceSharingSnapshot` with pure values only:

```cpp
std::optional<ResourceOperation> foregroundOperation;
ResourceConsumerSurface consumerSurface{ResourceConsumerSurface::none};
std::optional<GuidKey> currentBaseId;
double lastCatalogMilliseconds{};
double lastUnionMilliseconds{};
bool safetyDisabled{};
```

- [ ] **Step 1: Publish only on observable change**

Continue using the existing snapshot dirty flag. Do not publish every EngineTick. Mark dirty only when:

- toggle/runtime phase changes;
- base/container counts change;
- foreground owner changes;
- union opens/restores;
- current base GUID changes;
- capability/error changes;
- timing sample changes after a completed catalog/union operation.

- [ ] **Step 2: Render diagnostic status**

The GUI shows:

- discovered same-guild base count, including terminal-only bases;
- ordinary loaded container count;
- crafting/building/repair capability independently;
- current foreground operation and single consumer surface;
- whether current base is known;
- last catalog/union duration;
- safety-disable/error message.

Do not add an inventory quantity preview cache or slot count.

- [ ] **Step 3: Keep logging sparse**

Allowed Verbose logs:

- catalog reconciliation completion;
- union application completion;
- union restoration completion;
- one capability/safety-disable transition.

Forbidden:

- logging in recipe/list/touch hooks;
- logging every EngineTick;
- logging every invalidation before coalescing;
- logging every array entry.

- [ ] **Step 4: Build and commit**

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditor
git diff --check
git add mods/PalworldEditor/inc/base_resource_sharing/pal_base_resources.hpp `
  mods/PalworldEditor/src/pal_base_resources.cpp `
  mods/PalworldEditor/src/dllmain.cpp
git commit -m "feat: expose bounded resource runtime diagnostics"
```

---

## Task 8: Update Version and Architecture Documentation

**Files:**

- Modify: `mods/PalworldEditor/src/dllmain.cpp`
- Modify: `README.md`
- Modify: `AGENTS.md`
- Modify: `CLAUDE.md`

- [ ] **Step 1: Update version only after automated gates pass**

Change the mod metadata and visible GUI title from `1.6.7` to `1.6.8`.

- [ ] **Step 2: Document the single-surface model**

State:

- crafting injects unique ordinary chest containers only into the local inventory helper;
- building injects them only into the current base’s storage module;
- operations preempt rather than combine;
- post-`OnRep` multiplicity is verified and invalid state rolls back;
- crafting release uses an idle lease, not `StartProduction` post;
- terminal-only bases count as bases but contribute no resource until a chest module exists.

- [ ] **Step 3: Document first-open and performance contracts**

Add the exact release gates from this plan to manual validation. Remove obsolete claims about:

- all base modules being expanded;
- combined helper/module targets;
- concurrent material sessions;
- immediate crafting release.

- [ ] **Step 4: Verify references**

```powershell
rg -n "UnionTargets|combine_union_targets|required_targets|all.*modules|所有据点.*模块|1\\.6\\.7" `
  README.md AGENTS.md CLAUDE.md mods/PalworldEditor
git diff --check
```

Expected: no stale architecture reference or old runtime version remains.

- [ ] **Step 5: Commit**

```powershell
git add mods/PalworldEditor/src/dllmain.cpp README.md AGENTS.md CLAUDE.md
git commit -m "docs: release PalworldEditor 1.6.8"
```

---

## Task 9: Automated Verification

**Files:**

- Verify only.

- [ ] **Step 1: Run the full required build**

From an x64 VS 2022 developer environment:

```powershell
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor `
  PalworldEditorTests PalworldEditorBaseResourceSharingTests
```

Expected: all targets succeed.

- [ ] **Step 2: Run all CTest tests**

```powershell
ctest --test-dir build --output-on-failure
```

Expected: all tests pass.

- [ ] **Step 3: Run structural scans**

```powershell
rg -n "UnionTargets|combine_union_targets|contains_union_targets|required_targets|editor_settings" `
  mods/PalworldEditor
rg -n "FindAllOf|ItemSlotArray|StackCount" `
  mods/PalworldEditor/src/pal_base_resource_runtime.cpp `
  mods/PalworldEditor/src/pal_base_resources.cpp
git diff --check
```

Expected:

- no old combined-target/session API;
- no forbidden global scan or slot mutation in the resource module;
- no whitespace errors.

- [ ] **Step 4: Inspect callback hot paths**

Manually inspect every `HookEvent::touch` branch and confirm it contains only fixed-size state updates. Inspect
EngineTick and confirm feature-off returns before scheduler/discovery/union work.

- [ ] **Step 5: Deploy**

```powershell
cmake --build --preset ninja-msvc-x64 --target deploy
```

Expected: the configured game installation receives the new `main.dll`.

---

## Task 10: In-Game Correctness and Performance Verification

**Files:**

- Game validation evidence only.

- [ ] **Step 1: Establish a clean baseline**

Disable IntegratedStorage, UBIM Lite, BlueprintResearch, and every equivalent resource-path mod. Restart the
game so the process-local resource switch begins off.

Measure:

- five minutes idle in the test base;
- repeatedly opening building and furnace menus;
- building-preview camera movement.

Record average FPS, 1% low, and representative frame-time spikes.

- [ ] **Step 2: Verify feature-off zero work**

With sharing off:

- no resource hooks remain registered;
- no catalog reconciliation or union log appears;
- opening B/furnace behaves like original Palworld;
- frame timing is indistinguishable from baseline within normal run-to-run noise.

- [ ] **Step 3: Verify first direct furnace open**

Restart or re-enter the world with sharing desired on. Without pressing B:

1. open a furnace/kitchen directly;
2. inspect cake ingredients;
3. confirm local eggs `2` plus remote eggs `30` displays `32`;
4. confirm max craft count is `4`;
5. start `4`;
6. wait for all `4` outputs;
7. confirm the fifth cannot be queued.

Failure conditions:

- `62` eggs;
- max `7`;
- output stops before `4`;
- the menu requires pressing B first.

- [ ] **Step 4: Verify B then furnace does not duplicate**

1. press B and inspect build eligibility;
2. exit building mode;
3. open the furnace;
4. repeat the `2 + 30` cake scenario.

Expected: still `32`, max `4`, actual `4`.

- [ ] **Step 5: Verify first building menu eligibility**

Enter a world/base with local materials insufficient but same-guild remote ordinary chests sufficient:

1. do not open a box, furnace, or workstation;
2. press B;
3. select an original Palworld building;
4. verify its icon is eligible on this first open;
5. place and build it;
6. verify remote materials are consumed exactly once.

- [ ] **Step 6: Verify operation preemption**

Rapidly alternate:

- furnace open;
- B/building mode;
- furnace open again.

For each transition, logs must show one matched restore followed by one apply. There must never be an active
helper-plus-module union or two live ledger entries.

- [ ] **Step 7: Verify the fifth terminal-only base**

On a five-base save where the fifth begins with only a terminal:

- GUI count is `5` bases;
- the fifth contributes `0` containers;
- add one ordinary chest;
- wait for the structural invalidation/reconciliation;
- container count increases once;
- its resources become available without toggling the feature;
- no full-frame hitch is introduced.

- [ ] **Step 8: Verify dynamic toggle and LoadMap restoration**

Repeat:

1. enable;
2. open craft/build union;
3. disable while active;
4. confirm original behavior and matched restoration;
5. re-enable in the same world;
6. confirm one initial reconcile and nonzero counts;
7. exit/re-enter the save;
8. confirm no stale union and correct reinitialization;
9. restart game and confirm off.

- [ ] **Step 9: Verify performance gates**

From logs and frame-time capture:

- catalog `< 1.0 ms`;
- crafting union `< 1.0 ms`;
- building union `<= 16.7 ms`;
- no repeated union/reconcile while a menu remains active;
- no periodic hitch/log spam over ten minutes idle;
- creation preview does not regress relative to off baseline after the one acquire frame.

If the current-base module union exceeds `16.7 ms`, stop release work and profile the single
`OnRep_ContainerInfos` call. Do not restore the old all-modules implementation and do not raise the gate.

- [ ] **Step 10: Verify restoration integrity**

After craft/build/off/LoadMap cycles:

- original local container order remains;
- runtime-added non-mod entries remain;
- no injected remote ID remains after restore;
- every successful apply has one successful restore;
- a failed validation produces rollback and a current-world safety disable, not a crash.

- [ ] **Step 11: Final repository status**

```powershell
git status --short --branch
git log --oneline --decorate -12
```

Expected: only intentional implementation/documentation commits and the pre-existing untracked user plan.

---

## Plan Self-Review Checklist

- [ ] The plan removes the exact double-counting path: no operation can inject the same container into both a base module and the player helper.
- [ ] The plan removes the exact partial-production path: `StartProduction` post no longer restores immediately.
- [ ] The first B/furnace eligibility callback has an explicit pre-original bootstrap path.
- [ ] Current-base selection uses GUID-only cross-frame state and exact Palworld function paths; no UObject pointer is retained.
- [ ] Terminal-only bases are counted without pretending they contain resources.
- [ ] Reconciliation is coalesced, paused during active union, and bounded by the existing scheduler.
- [ ] High-frequency callbacks are O(1) and log-free.
- [ ] Every mutation has a post-`OnRep` multiplicity check and verified rollback.
- [ ] Performance and correctness thresholds are numeric and block the version bump on failure.
- [ ] Automated RED/GREEN commands and game validation steps reproduce the user’s screenshots: `2 + 30`, `62`, max `7`, actual `4`.
- [ ] No `TODO`, `TBD`, “similar to”, unspecified Hook, or permissive “implement as needed” instruction remains.
