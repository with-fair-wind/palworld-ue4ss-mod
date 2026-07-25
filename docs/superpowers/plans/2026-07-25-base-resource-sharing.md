# PalworldEditor Base Resource Sharing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a persisted, default-off switch that lets standalone/listen-host players build and craft from loaded normal storage containers across every base owned by their guild, while preserving vanilla consumption and failing closed for unsupported repair flows.

**Architecture:** Add standard-library-only settings, capability, resource-pool, and union-ledger components, then place all Palworld reflection and UFunction hooks behind a focused `PalBaseResourceBridge`. For one request, the bridge temporarily appends GUID-deduplicated guild containers to Palworld's existing storage-module/helper arrays, lets Palworld perform its normal validation and physical consumption, then restores and verifies the exact original arrays. Crafting restores in the same hooked call; building uses a bounded 750 ms game-thread window established by the authoritative request hook.

**Tech Stack:** C++23, CMake 3.22+, Ninja, MSVC, UE4SS experimental C++ API, PalSchema, ImGui, CTest.

## Global Constraints

- Target Palworld 1.0/1.0.1 with the experimental UE4SS + PalSchema runtime described in `AGENTS.md`.
- Support standalone and local listen-host play only; do not claim remote-client or dedicated-server support.
- Keep the switch default-off and persist only `Enabled=true|false` in `ue4ss/Mods/PalworldEditor/config.ini`.
- Do not merge storage UI, move items, create a virtual inventory, alter Pal transport/food/automatic production, or trust a client-supplied guild/container ID.
- Do not write `UPalItemSlot::StackCount`; Palworld must perform physical consumption, replication, and persistence.
- Execute every UObject lookup, reflection access, hook body, union mutation, and restoration on the Unreal game thread.
- Reuse the existing EngineTick and LoadMap lifecycle; restore/clear an active union before a world becomes inaccessible.
- Cache no player, base, storage-module, helper, or item-container instance beyond the bounded active request. Persistent hook state may retain only validated `UFunction` metadata plus callback IDs.
- Treat construction, crafting, and repair as independent capabilities. Construction/crafting use the Palworld 1.0.1 paths below; repair remains unavailable with an explicit diagnostic until a safe check+consume pair is proven.
- Implement independently. The public UBIM Lite repository is behavioral evidence for Palworld 1.0.1 function paths and timing, not a source from which to copy code.
- Do not deploy to the game directory as part of this plan unless the user separately authorizes deployment.
- Follow `cpp-pro`, `cpp-coding-standards`, `modern-cpp-coding-standards`, and the repository formatting rules.

## Runtime Evidence to Read Before Implementation

- Design: `docs/superpowers/specs/2026-07-25-base-resource-sharing-design.md`
- Current lifecycle: `mods/PalworldEditor/src/dllmain.cpp`
- Current reflection helpers: `mods/PalworldEditor/inc/game/pal_game.hpp`
- PalSchema: <https://github.com/Okaetsu/PalSchema>
- Palworld 1.0.1 behavior and function-path evidence:
  <https://github.com/AuronNetwork/UBIM-Lite/blob/main/Documentation/NativeResourceUnion_v1.0.md>
- UBIM Lite reports that crafting/building can consume from a request-local union of real containers while Palworld remains responsible for the physical write. Reproduce that behavior with this repository's architecture and naming; do not copy its implementation.

## File Structure

Create:

- `mods/PalworldEditor/inc/base_resource_sharing/settings.hpp` — pure settings values, parsing, serialization, and file-I/O declarations.
- `mods/PalworldEditor/src/base_resource_settings.cpp` — atomic Windows settings load/save implementation.
- `mods/PalworldEditor/inc/base_resource_sharing/resource_pool.hpp` — pure GUID, container filtering/order, capability, and union-ledger logic.
- `mods/PalworldEditor/inc/base_resource_sharing/hook_manifest.hpp` — exact Palworld 1.0.1 hook paths and pure capability evaluation.
- `mods/PalworldEditor/inc/base_resource_sharing/pal_base_resources.hpp` — game-thread bridge public interface and value-only GUI snapshot.
- `mods/PalworldEditor/src/pal_base_resources.cpp` — all Palworld base/container reflection, transient union logic, hooks, and restoration.
- `mods/PalworldEditor/tests/base_resource_sharing_tests.cpp` — standalone settings/resource/capability/lifecycle tests.

Modify:

- `mods/PalworldEditor/CMakeLists.txt` — compile the new runtime sources and test executable.
- `mods/PalworldEditor/src/dllmain.cpp` — lifecycle integration, GUI request handoff, persisted toggle, and version.
- `README.md` — usage, limitations, conflict warning, and validation procedure.
- `AGENTS.md` — architecture, version, and game-thread/resource-union contract.

---

### Task 1: Persisted Default-Off Settings

**Files:**
- Create: `mods/PalworldEditor/inc/base_resource_sharing/settings.hpp`
- Create: `mods/PalworldEditor/src/base_resource_settings.cpp`
- Create: `mods/PalworldEditor/tests/base_resource_sharing_tests.cpp`
- Modify: `mods/PalworldEditor/CMakeLists.txt`

**Interfaces:**
- Consumes: `std::filesystem`, Windows `MoveFileExW`, and the existing MSVC test configuration.
- Produces:
  - `base_resource_sharing::Settings`
  - `base_resource_sharing::SettingsParseResult`
  - `parse_settings(std::string_view) -> SettingsParseResult`
  - `serialize_settings(const Settings&) -> std::string`
  - `load_settings(const std::filesystem::path&) -> SettingsParseResult`
  - `save_settings(const std::filesystem::path&, const Settings&) -> std::string`

- [ ] **Step 1: Add the settings tests and new CTest target**

Start `base_resource_sharing_tests.cpp` with the same small `CHECK` harness used by
`skill_editor_tests.cpp`, then add:

```cpp
#include <base_resource_sharing/settings.hpp>

void test_settings_default_off_and_round_trip() {
    using namespace base_resource_sharing;

    const auto missing = parse_settings("");
    CHECK(!missing.settings.enabled);
    CHECK(!missing.error.empty());

    const auto enabled = parse_settings(
        "[BaseResourceSharing]\n"
        "Enabled=true\n");
    CHECK(enabled.settings.enabled);
    CHECK(enabled.error.empty());
    CHECK(serialize_settings(enabled.settings) ==
          "[BaseResourceSharing]\nEnabled=true\n");

    const auto invalid = parse_settings(
        "[BaseResourceSharing]\n"
        "Enabled=maybe\n");
    CHECK(!invalid.settings.enabled);
    CHECK(!invalid.error.empty());
}
```

Register the test from `main()` and add:

```cmake
add_executable(PalworldEditorBaseResourceSharingTests
    tests/base_resource_sharing_tests.cpp
    src/base_resource_settings.cpp
)
target_include_directories(PalworldEditorBaseResourceSharingTests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc)
target_compile_features(PalworldEditorBaseResourceSharingTests PRIVATE cxx_std_23)
if(MSVC)
    target_compile_options(PalworldEditorBaseResourceSharingTests
        PRIVATE /utf-8 /W4 /permissive-)
endif()
add_test(NAME PalworldEditor.BaseResourceSharing
    COMMAND PalworldEditorBaseResourceSharingTests)
```

Also add `src/base_resource_settings.cpp` to `PalworldEditor`.

- [ ] **Step 2: Run the new test target and verify RED**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorBaseResourceSharingTests
```

Expected: compilation fails because `base_resource_sharing/settings.hpp` and its functions do not exist.

- [ ] **Step 3: Declare the settings API**

Create:

```cpp
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace base_resource_sharing {
struct Settings {
    bool enabled{};
};

struct SettingsParseResult {
    Settings settings;
    std::string error;
};

auto parse_settings(std::string_view text) -> SettingsParseResult;
auto serialize_settings(const Settings& settings) -> std::string;
auto load_settings(const std::filesystem::path& path) -> SettingsParseResult;
auto save_settings(const std::filesystem::path& path, const Settings& settings) -> std::string;
}  // namespace base_resource_sharing
```

Implement strict parsing:

- accept only section `[BaseResourceSharing]`;
- accept only `Enabled=true` or `Enabled=false`, case-insensitively after ASCII trimming;
- duplicate `Enabled` keys, malformed lines, missing section/key, or any other value return `enabled=false` with a specific error;
- serialization always emits UTF-8 LF text exactly as asserted above.

Implement `load_settings` so a missing file returns default-off plus
`"配置文件不存在，已使用默认关闭。"` without creating a file.

Implement `save_settings` by:

1. creating the parent directory;
2. writing `<path>.tmp` in binary/truncate mode;
3. flushing and closing it;
4. calling `MoveFileExW(temp, destination, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`;
5. returning an empty string on success or a Chinese error containing the Win32 error code on failure.

- [ ] **Step 4: Add an atomic-file round-trip test**

Use a unique directory under `std::filesystem::temp_directory_path()`:

```cpp
void test_settings_file_round_trip() {
    using namespace base_resource_sharing;

    const auto root = std::filesystem::temp_directory_path() /
                      "PalworldEditorBaseResourceSharingTests";
    const auto path = root / "config.ini";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);

    CHECK(save_settings(path, Settings{.enabled = true}).empty());
    const auto loaded = load_settings(path);
    CHECK(loaded.settings.enabled);
    CHECK(loaded.error.empty());

    std::filesystem::remove_all(root, ignored);
}
```

The test owns only its exact named temporary directory and removes it before and after use.

- [ ] **Step 5: Run tests and verify GREEN**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorBaseResourceSharingTests
ctest --test-dir build -R PalworldEditor.BaseResourceSharing --output-on-failure
```

Expected: the new test builds and passes.

- [ ] **Step 6: Commit**

```powershell
git add mods/PalworldEditor/CMakeLists.txt `
        mods/PalworldEditor/inc/base_resource_sharing/settings.hpp `
        mods/PalworldEditor/src/base_resource_settings.cpp `
        mods/PalworldEditor/tests/base_resource_sharing_tests.cpp
git commit -m "feat: persist base resource sharing preference"
```

---

### Task 2: Pure Resource Pool, Ordering, and World State

**Files:**
- Create: `mods/PalworldEditor/inc/base_resource_sharing/resource_pool.hpp`
- Modify: `mods/PalworldEditor/tests/base_resource_sharing_tests.cpp`

**Interfaces:**
- Consumes: Task 1's test harness.
- Produces:
  - `GuidKey`
  - `ContainerKind`
  - `ContainerDescriptor`
  - `ResourceUnionPlan`
  - `make_resource_union_plan(...)`
  - `ResourceOperation`
  - `CapabilityState`
  - `RuntimeState`

- [ ] **Step 1: Add failing pool/order tests**

```cpp
#include <base_resource_sharing/resource_pool.hpp>

void test_resource_pool_filters_deduplicates_and_orders() {
    using namespace base_resource_sharing;
    const GuidKey guild{{1, 0, 0, 0}};
    const GuidKey otherGuild{{2, 0, 0, 0}};
    const GuidKey baseA{{10, 0, 0, 0}};
    const GuidKey baseB{{20, 0, 0, 0}};

    const std::vector<ContainerDescriptor> containers{
        {.baseId = baseB, .groupId = guild, .containerId = {{202, 0, 0, 0}},
         .kind = ContainerKind::normal},
        {.baseId = baseA, .groupId = guild, .containerId = {{101, 0, 0, 0}},
         .kind = ContainerKind::normal, .currentBase = true},
        {.baseId = baseA, .groupId = guild, .containerId = {{101, 0, 0, 0}},
         .kind = ContainerKind::normal, .currentBase = true},
        {.baseId = baseA, .groupId = guild, .containerId = {{102, 0, 0, 0}},
         .kind = ContainerKind::food, .currentBase = true},
        {.baseId = baseB, .groupId = otherGuild, .containerId = {{203, 0, 0, 0}},
         .kind = ContainerKind::normal},
    };

    const auto plan = make_resource_union_plan(containers, guild);
    CHECK(plan.error.empty());
    CHECK(plan.baseCount == 2);
    CHECK(plan.ordered.size() == 2);
    CHECK(plan.ordered[0].containerId == GuidKey{{101, 0, 0, 0}});
    CHECK(plan.ordered[1].containerId == GuidKey{{202, 0, 0, 0}});
}
```

Also test invalid guild IDs and an empty result return an error and no containers.

- [ ] **Step 2: Add failing lifecycle/capability tests**

```cpp
void test_runtime_state_fails_closed_across_worlds() {
    using namespace base_resource_sharing;
    RuntimeState state;
    state.set_preference(true);
    state.finish_world_transition(7);
    state.set_capability(
        ResourceOperation::crafting,
        CapabilityState{.previewReady = true, .consumeReady = true});

    CHECK(state.can_extend(ResourceOperation::crafting, 7));
    CHECK(!state.can_extend(ResourceOperation::building, 7));

    state.begin_world_transition(8);
    CHECK(!state.can_extend(ResourceOperation::crafting, 7));
    CHECK(!state.can_extend(ResourceOperation::crafting, 8));

    state.finish_world_transition(8);
    CHECK(!state.can_extend(ResourceOperation::crafting, 8));
}
```

The final assertion proves capabilities must be re-published after LoadMap.

- [ ] **Step 3: Run tests and verify RED**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorBaseResourceSharingTests
```

Expected: missing `resource_pool.hpp` and undefined types/functions.

- [ ] **Step 4: Implement the value types and algorithms**

Use:

```cpp
namespace base_resource_sharing {
struct GuidKey {
    std::array<std::uint32_t, 4> words{};
    [[nodiscard]] auto valid() const noexcept -> bool;
    auto operator<=>(const GuidKey&) const = default;
};

enum class ContainerKind { normal, food, player, other };

struct ContainerDescriptor {
    GuidKey baseId;
    GuidKey groupId;
    GuidKey containerId;
    ContainerKind kind{ContainerKind::other};
    bool currentBase{};
};

struct ResourceUnionPlan {
    std::vector<ContainerDescriptor> ordered;
    std::size_t baseCount{};
    std::string error;
};

auto make_resource_union_plan(
    std::span<const ContainerDescriptor> containers,
    const GuidKey& currentGuild) -> ResourceUnionPlan;

enum class ResourceOperation : std::uint8_t { crafting, building, repair };

struct CapabilityState {
    bool previewReady{};
    bool consumeReady{};
    std::string error;
    [[nodiscard]] auto available() const noexcept -> bool;
};

class RuntimeState {
public:
    auto set_preference(bool enabled) noexcept -> void;
    auto begin_world_transition(std::uint64_t generation) -> void;
    auto finish_world_transition(std::uint64_t generation) -> void;
    auto set_capability(ResourceOperation operation, CapabilityState capability) -> void;
    [[nodiscard]] auto can_extend(
        ResourceOperation operation, std::uint64_t generation) const -> bool;
    [[nodiscard]] auto generation() const noexcept -> std::uint64_t;
    [[nodiscard]] auto enabled() const noexcept -> bool;
    [[nodiscard]] auto capability(ResourceOperation operation) const -> const CapabilityState&;
};
}  // namespace base_resource_sharing
```

Algorithm requirements:

- reject an invalid guild;
- retain only `normal`, valid, same-guild descriptors;
- sort current-base descriptors first, then by `baseId`, then `containerId`;
- deduplicate by `containerId`;
- compute unique base count after filtering;
- `RuntimeState::finish_world_transition` marks the world accessible but resets every capability;
- `can_extend` requires preference enabled, accessible world, exact generation, and a complete capability.

- [ ] **Step 5: Run tests and verify GREEN**

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorBaseResourceSharingTests
ctest --test-dir build -R PalworldEditor.BaseResourceSharing --output-on-failure
```

Expected: all settings and resource-pool tests pass.

- [ ] **Step 6: Commit**

```powershell
git add mods/PalworldEditor/inc/base_resource_sharing/resource_pool.hpp `
        mods/PalworldEditor/tests/base_resource_sharing_tests.cpp
git commit -m "feat: model guild resource union state"
```

---

### Task 3: Exact Hook Manifest and Fail-Closed Capabilities

**Files:**
- Create: `mods/PalworldEditor/inc/base_resource_sharing/hook_manifest.hpp`
- Modify: `mods/PalworldEditor/tests/base_resource_sharing_tests.cpp`

**Interfaces:**
- Consumes: `ResourceOperation` and `CapabilityState` from Task 2.
- Produces:
  - `HookRole`
  - `HookSpec`
  - `HookResolution`
  - `palworld_1_0_1_hook_manifest()`
  - `all_hook_resolutions(bool resolved)`
  - `mark_resolved(std::span<HookResolution>, std::string_view path)`
  - `operation_index(ResourceOperation)`
  - `evaluate_capabilities(std::span<const HookResolution>)`

- [ ] **Step 1: Add failing manifest tests**

```cpp
#include <base_resource_sharing/hook_manifest.hpp>

void test_hook_capabilities_require_preview_and_consume_paths() {
    using namespace base_resource_sharing;
    auto resolved = all_hook_resolutions(false);

    mark_resolved(resolved,
        "/Script/Pal.PalUIProductSettingModel:CalcMaxProductableNum");
    mark_resolved(resolved,
        "/Script/Pal.PalUIConvertItemModel:StartProduction");
    auto capabilities = evaluate_capabilities(resolved);
    CHECK(capabilities[operation_index(ResourceOperation::crafting)].available());
    CHECK(!capabilities[operation_index(ResourceOperation::building)].available());
    CHECK(!capabilities[operation_index(ResourceOperation::repair)].available());

    mark_resolved(resolved,
        "/Script/Pal.PalBuilderComponent:IsExistsMaterialForBuildObject");
    mark_resolved(resolved,
        "/Script/Pal.PalNetworkPlayerComponent:RequestBuild_ToServer");
    capabilities = evaluate_capabilities(resolved);
    CHECK(capabilities[operation_index(ResourceOperation::building)].available());
    CHECK(capabilities[operation_index(ResourceOperation::repair)].error ==
          "Palworld 1.0.1 尚未验证安全的修理材料检查与扣除入口。");
}
```

- [ ] **Step 2: Run tests and verify RED**

Expected: missing `hook_manifest.hpp`.

- [ ] **Step 3: Implement the manifest**

Define required hooks:

```cpp
enum class HookRole : std::uint8_t { preview, consume, uiConsistency };

struct HookSpec {
    ResourceOperation operation;
    HookRole role;
    std::string_view path;
};

struct HookResolution {
    HookSpec spec;
    bool resolved{};
};

constexpr auto operation_index(ResourceOperation operation) noexcept -> std::size_t {
    return static_cast<std::size_t>(operation);
}

auto all_hook_resolutions(bool resolved) -> std::vector<HookResolution>;
void mark_resolved(std::span<HookResolution> resolutions, std::string_view path);
auto evaluate_capabilities(std::span<const HookResolution> resolutions)
    -> std::array<CapabilityState, 3>;

inline constexpr HookSpec kRequiredHooks[] = {
    {ResourceOperation::crafting, HookRole::preview,
     "/Script/Pal.PalUIProductSettingModel:CalcMaxProductableNum"},
    {ResourceOperation::crafting, HookRole::consume,
     "/Script/Pal.PalUIConvertItemModel:StartProduction"},
    {ResourceOperation::building, HookRole::preview,
     "/Script/Pal.PalBuilderComponent:IsExistsMaterialForBuildObject"},
    {ResourceOperation::building, HookRole::consume,
     "/Script/Pal.PalNetworkPlayerComponent:RequestBuild_ToServer"},
};
```

Define optional UI consistency hooks:

```cpp
inline constexpr std::string_view kOptionalUiHooks[] = {
    "/Script/Pal.PalBuilderComponent:CollectItemInfoEnableToUseMaterial",
    "/Script/Pal.PalUIConvertItemModel:CanStartProduction",
    "/Script/Pal.PalItemContainerMultiHelper:IsExistItems",
    "/Script/Pal.PalItemContainerMultiHelper:FindByStaticItemIds",
    "/Script/Pal.PalItemContainerMultiHelper:FindByStaticItemId",
    "/Script/Pal.PalItemUtility:CountLocalPlayerInsideBaseCampItemNum64",
    "/Script/Pal.PalItemUtility:CountLocalPlayerAndInsideBaseCampItemNum64",
    "/Script/Pal.PalItemUtility:CollectLocalPlayerControllableItemInfos",
    "/Script/Pal.PalItemUtility:CollectLocalPlayerControllableAllItemInfos",
};
```

Capability evaluation rules:

- construction/crafting are available only if both required roles resolve;
- missing optional hooks produce a warning but do not enable an unsafe consume path;
- repair always reports the exact unsupported diagnostic from the test;
- error strings list missing required paths verbatim.

- [ ] **Step 4: Run tests and verify GREEN**

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorBaseResourceSharingTests
ctest --test-dir build -R PalworldEditor.BaseResourceSharing --output-on-failure
```

- [ ] **Step 5: Commit**

```powershell
git add mods/PalworldEditor/inc/base_resource_sharing/hook_manifest.hpp `
        mods/PalworldEditor/tests/base_resource_sharing_tests.cpp
git commit -m "feat: define Palworld resource hook capabilities"
```

---

### Task 4: Read-Only Guild Storage Discovery

**Files:**
- Create: `mods/PalworldEditor/inc/base_resource_sharing/pal_base_resources.hpp`
- Create: `mods/PalworldEditor/src/pal_base_resources.cpp`
- Modify: `mods/PalworldEditor/CMakeLists.txt`
- Modify: `mods/PalworldEditor/tests/base_resource_sharing_tests.cpp`

**Interfaces:**
- Consumes: Tasks 2–3 value types and exact function paths.
- Produces:
  - `BaseResourceSharingSnapshot`
  - `PalBaseResourceBridge::discover_loaded_guild_storage()`
  - `PalBaseResourceBridge::snapshot()`

- [ ] **Step 1: Add a failing completeness test for discovery input**

Add a pure helper test:

```cpp
void test_discovery_rejects_partially_resolved_container_sets() {
    using namespace base_resource_sharing;
    const GuidKey guild{{1, 0, 0, 0}};
    const std::vector<ContainerDescriptor> registered{
        {.baseId = {{10, 0, 0, 0}}, .groupId = guild,
         .containerId = {{101, 0, 0, 0}}, .kind = ContainerKind::normal},
        {.baseId = {{20, 0, 0, 0}}, .groupId = guild,
         .containerId = {{201, 0, 0, 0}}, .kind = ContainerKind::normal},
    };
    const std::array firstOnly{registered[0].containerId};
    const std::array both{registered[0].containerId, registered[1].containerId};

    CHECK(validate_live_container_resolution(registered, firstOnly)
              .error == "仅解析到 1/2 个已登记据点资源容器。");
    CHECK(validate_live_container_resolution(registered, both)
              .error.empty());
}
```

Declare/implement
`validate_live_container_resolution(std::span<const ContainerDescriptor>,
std::span<const GuidKey>) -> ValidationResult` in `resource_pool.hpp`. A partial result must never form a union.

- [ ] **Step 2: Run tests and verify RED, then implement the pure validator**

Run the resource test target; expect undefined `validate_live_container_resolution`. Implement it and rerun until GREEN.

- [ ] **Step 3: Declare the bridge API**

Use a focused interface:

```cpp
namespace base_resource_sharing {
struct BaseResourceSharingSnapshot {
    bool enabled{};
    bool worldAccessible{};
    std::uint64_t worldGeneration{};
    std::size_t baseCount{};
    std::size_t containerCount{};
    std::array<CapabilityState, 3> capabilities;
    std::string status;
    std::string configError;
};

class PalBaseResourceBridge final {
public:
    PalBaseResourceBridge();
    ~PalBaseResourceBridge();
    PalBaseResourceBridge(const PalBaseResourceBridge&) = delete;
    auto operator=(const PalBaseResourceBridge&) -> PalBaseResourceBridge& = delete;

    auto set_enabled(bool enabled) -> void;
    auto on_world_begin(std::uint64_t generation) -> void;
    auto on_world_ready(std::uint64_t generation) -> void;
    auto tick(float deltaSeconds) -> void;
    auto ensure_hooks_registered() -> void;
    auto shutdown_hooks() -> void;
    [[nodiscard]] auto snapshot() const -> BaseResourceSharingSnapshot;
};
}  // namespace base_resource_sharing
```

Keep Unreal-heavy details in a private implementation owned by `std::unique_ptr<Impl>`.

- [ ] **Step 4: Implement read-only reflection discovery**

In `pal_base_resources.cpp`, independently implement:

1. Resolve local controller with
   `/Script/Pal.Default__PalUtility:GetLocalPalPlayerController`.
2. Call controller `GetPlayerUId`.
3. Call `/Script/Pal.Default__PalUtility:GetGuildByPlayerUId`.
4. Call guild `GetId`.
5. `FindAllOf("PalBaseCampModuleItemStorage")`.
6. For every module, get typed outer `/Script/Pal.PalBaseCampModel`.
7. Call `GetGroupIdBelongTo` and retain only the local guild.
8. Read array property `ContainerInfos`.
9. For each entry, navigate reflected structs
   `ContainerIdCache -> ID -> FGuid`.
10. Find loaded `PalItemContainer` objects, call `GetId`, and require an exact GUID match for every registered normal container.
11. Convert all GUIDs to `GuidKey`; publish only value snapshots.

Do not use old integer values for `EPalBaseCampItemContainerType`. The `ContainerInfos` array itself is the Palworld 1.0.1 normal-storage registry; do not include separate guild, food, player, or scripted arrays.

Add `src/pal_base_resources.cpp` to `PalworldEditor`. At this stage hooks remain unregistered and all capabilities remain unavailable.

- [ ] **Step 5: Compile the DLL and run all tests**

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditor PalworldEditorTests PalworldEditorBaseResourceSharingTests
ctest --test-dir build --output-on-failure
```

Expected: DLL links and both CTest cases pass.

- [ ] **Step 6: Commit**

```powershell
git add mods/PalworldEditor/CMakeLists.txt `
        mods/PalworldEditor/inc/base_resource_sharing/pal_base_resources.hpp `
        mods/PalworldEditor/inc/base_resource_sharing/resource_pool.hpp `
        mods/PalworldEditor/src/pal_base_resources.cpp `
        mods/PalworldEditor/tests/base_resource_sharing_tests.cpp
git commit -m "feat: discover guild base storage"
```

---

### Task 5: Verified Transient Union and Exact Restoration

**Files:**
- Modify: `mods/PalworldEditor/inc/base_resource_sharing/resource_pool.hpp`
- Modify: `mods/PalworldEditor/src/pal_base_resources.cpp`
- Modify: `mods/PalworldEditor/tests/base_resource_sharing_tests.cpp`

**Interfaces:**
- Consumes: Task 4 discovery results.
- Produces:
  - `ArrayPatchLedger`
  - `verify_restoration_sequence(...)`
  - private bridge methods `begin_union(...)` and `restore_union(...)`

- [ ] **Step 1: Add failing ledger/restoration tests**

```cpp
void test_union_restoration_accepts_only_the_recorded_tail() {
    using namespace base_resource_sharing;
    const GuidKey a{{1, 0, 0, 0}};
    const GuidKey b{{2, 0, 0, 0}};
    const GuidKey c{{3, 0, 0, 0}};
    const std::array original{a};
    const std::array correctCurrent{a, b, c};
    const std::array reorderedCurrent{a, c, b};
    const std::array missingPrefix{b, c};
    const std::array appended{b, c};

    CHECK(verify_restoration_sequence(original, correctCurrent, appended));
    CHECK(!verify_restoration_sequence(original, reorderedCurrent, appended));
    CHECK(!verify_restoration_sequence(original, missingPrefix, appended));
}
```

Add a second test proving current entries remain the prefix and only missing GUIDs are appended:

```cpp
const std::array firstPlan{a};
const std::array partialPlan{a, c};
const std::array globalPlan{a, b, c};
CHECK(missing_union_tail(firstPlan, globalPlan) == std::vector<GuidKey>({b, c}));
CHECK(missing_union_tail(partialPlan, globalPlan) == std::vector<GuidKey>({b}));
```

- [ ] **Step 2: Run tests and verify RED**

Expected: undefined ledger helpers.

- [ ] **Step 3: Implement the pure ledger**

```cpp
struct ArrayPatchLedger {
    std::wstring objectFullName;
    std::vector<GuidKey> original;
    std::vector<GuidKey> appended;
    bool helperArray{};
};
```

`verify_restoration_sequence` requires:

- `current.size() == original.size() + appended.size()`;
- `current` begins with `original`;
- its tail exactly equals `appended`.

`missing_union_tail` preserves global plan order and removes IDs already present in the target.
Declare both helpers with `std::span<const GuidKey>` parameters so the tests and bridge share one exact interface.

- [ ] **Step 4: Implement independent Unreal array append/remove helpers**

In the bridge:

- reflect `FArrayProperty::GetInner()`;
- append with `FScriptArray::Add`, `InitializeValue`, and `CopyCompleteValue`;
- remove only an exact verified tail by calling `DestroyValue` on each appended element and `FScriptArray::Remove`;
- never resize `ItemSlotArray` or write item counts.

For each guild storage module:

1. record object full name and original GUID order;
2. append copied `FPalBaseCampItemContainerInfo` entries missing from that module;
3. record the exact appended GUID order.

For each active `PalItemContainerMultiHelper` participating in the current request:

1. record full name and original container GUID order;
2. append missing live `UPalItemContainer*` values;
3. record the exact appended order.

If any append fails, immediately restore every earlier patch in reverse order and mark the operation unavailable for the current world.

Restoration re-resolves objects by full name on the game thread, re-reflects the array property, verifies the original prefix/appended tail, removes only the recorded tail, and reads the final GUID sequence back. A vanished transient helper is considered already gone; a live object with a mismatched sequence is an error and disables sharing until the next world.

- [ ] **Step 5: Add reentrancy and LoadMap behavior**

- reject a second union while one is active;
- assign every union the current `worldGeneration`;
- make `on_world_begin` call `restore_union()` before clearing state;
- after restoration, clear all ledgers and capability flags;
- a generation mismatch never opens or publishes a union.

- [ ] **Step 6: Run tests/build and verify GREEN**

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditor PalworldEditorBaseResourceSharingTests
ctest --test-dir build -R PalworldEditor.BaseResourceSharing --output-on-failure
```

- [ ] **Step 7: Commit**

```powershell
git add mods/PalworldEditor/inc/base_resource_sharing/resource_pool.hpp `
        mods/PalworldEditor/src/pal_base_resources.cpp `
        mods/PalworldEditor/tests/base_resource_sharing_tests.cpp
git commit -m "feat: add reversible guild resource union"
```

---

### Task 6: Crafting Preview and Same-Call Consumption

**Files:**
- Modify: `mods/PalworldEditor/src/pal_base_resources.cpp`
- Modify: `mods/PalworldEditor/tests/base_resource_sharing_tests.cpp`

**Interfaces:**
- Consumes: Task 3 crafting paths and Task 5 union.
- Produces: crafting capability, preview totals, and same-call production union.

- [ ] **Step 1: Add a failing hook-state test**

Add a pure state transition around `RuntimeState`:

```cpp
void test_crafting_union_is_same_call_and_not_reentrant() {
    using namespace base_resource_sharing;
    RequestGuard guard;
    CHECK(guard.try_enter(ResourceOperation::crafting, 12));
    CHECK(!guard.try_enter(ResourceOperation::crafting, 12));
    guard.leave(ResourceOperation::crafting, 12);
    CHECK(!guard.active());
}
```

Define `RequestGuard` in `resource_pool.hpp`; it stores only operation/generation/depth, not UObject pointers.

- [ ] **Step 2: Run tests and verify RED, then implement `RequestGuard`**

Rerun until the pure test passes.

- [ ] **Step 3: Register crafting hooks once**

From `ensure_hooks_registered()` on EngineTick:

- resolve every path through `StaticFindObject<UFunction*>`;
- register each resolved hook only once;
- keep stable `UFunction*` metadata plus callback IDs solely for unregistration;
- evaluate capabilities from Task 3;
- do not repeat registration after LoadMap when the same UFunction identity remains.

Required crafting hooks:

- `PalUIProductSettingModel:CalcMaxProductableNum`;
- `PalUIConvertItemModel:StartProduction`.

Register resolved optional crafting/UI paths from `kOptionalUiHooks`.

- [ ] **Step 4: Implement read-only global crafting counts**

For the current local guild:

1. discover an exact loaded container set;
2. enumerate `UPalItemContainer::ItemSlotArray`;
3. read slot `ItemId.StaticId` and `StackCount`;
4. merge counts in `std::unordered_map<FName, std::int64_t>`;
5. reject dynamic/invalid IDs and negative counts;
6. use `PalUIProductSettingModel:GetRequiredMaterialInfos(OneUnit=true)` to calculate maximum producible quantity.

Post-hooks may increase the vanilla result but must never lower it:

- `CalcMaxProductableNum`: `max(vanilla, globalMax)`;
- `CanStartProduction`: change only `FailedNotEnoughItems` (`4`) to `Enable` (`0`) when the requested quantity is globally available;
- count/collect helper hooks replace only material counts/outputs, not recipe IDs or selected products.

Cache the read-only global count snapshot for 15 seconds for high-frequency UI calls. Invalidate it after production/build requests, storage-update diagnostics, LoadMap, or a failed exact read. Authoritative consume hooks always rediscover live containers and never trust this cache.

- [ ] **Step 5: Implement same-call crafting consumption**

In the pre-hook for `PalUIConvertItemModel:StartProduction`:

- verify enabled/world generation/crafting capability;
- resolve the local authority player and guild;
- open the transient module/helper union;
- remember that this exact UFunction owns the active synchronous union.

In its post-hook:

- restore and verify the union;
- clear the request guard;
- invalidate the preview cache;
- if restoration fails, disable crafting for the current world and publish the error.

Do not override the production success return and do not consume items manually.

- [ ] **Step 6: Build and run tests**

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditor PalworldEditorBaseResourceSharingTests
ctest --test-dir build --output-on-failure
git diff --check
```

- [ ] **Step 7: Commit**

```powershell
git add mods/PalworldEditor/inc/base_resource_sharing/resource_pool.hpp `
        mods/PalworldEditor/src/pal_base_resources.cpp `
        mods/PalworldEditor/tests/base_resource_sharing_tests.cpp
git commit -m "feat: share resources for crafting"
```

---

### Task 7: Building Preview and Bounded Authority Window

**Files:**
- Modify: `mods/PalworldEditor/src/pal_base_resources.cpp`
- Modify: `mods/PalworldEditor/tests/base_resource_sharing_tests.cpp`

**Interfaces:**
- Consumes: Task 3 building paths, Task 5 union, and EngineTick.
- Produces: building capability and the 750 ms authority-request union window.

- [ ] **Step 1: Add failing timeout-state tests**

```cpp
void test_build_union_timeout_is_bounded_and_generation_safe() {
    using namespace base_resource_sharing;
    BuildUnionWindow window;
    CHECK(window.open(42));
    CHECK(!window.advance(0.74F, 42));
    CHECK(window.advance(0.01F, 42));
    CHECK(!window.opened());

    CHECK(window.open(43));
    CHECK(window.advance(0.01F, 44));
    CHECK(!window.opened());
}
```

`advance` returns `true` when the caller must restore: at 0.75 seconds or on generation mismatch.

- [ ] **Step 2: Run tests and verify RED, then implement `BuildUnionWindow`**

Use a `float elapsed`, exact generation, and `constexpr float kBuildUnionTimeoutSeconds = 0.75F`.

- [ ] **Step 3: Implement building preview**

Required preview hook:

- `/Script/Pal.PalBuilderComponent:IsExistsMaterialForBuildObject`.

Read `BuildObjectData` reflected fields:

- `Material1_Id` / `Material1_Count`;
- `Material2_Id` / `Material2_Count`;
- `Material3_Id` / `Material3_Count`;
- `Material4_Id` / `Material4_Count`.

Merge repeated IDs with checked integer addition. In the post-hook, change `false` to `true` only when the complete global requirement is available.

For `CollectItemInfoEnableToUseMaterial` and
`CollectLocalPlayerControllableAllItemInfos`, publish the same global counts used by the build menu without changing raw item IDs.

- [ ] **Step 4: Implement authoritative build union**

Register:

```text
/Script/Pal.PalNetworkPlayerComponent:RequestBuild_ToServer
```

In its pre-hook:

1. require enabled/current generation/building capability;
2. derive authority identity from the request component's owner/transmitter/controller chain;
3. derive player UID and guild through `GetPlayerUId` and `GetGuildByPlayerUId`;
4. open a live union;
5. open `BuildUnionWindow` for 0.75 seconds;
6. reject a concurrent second union without altering the request.

Do not edit `FPalBuildRequestDebugParameter`; Palworld 1.0 Shipping evidence shows its no-consume flag is not a reliable interception mechanism.

In `PalBaseResourceBridge::tick(deltaSeconds)`:

- advance the timeout on the game thread;
- at expiry, restore/verify all module/helper ledgers;
- invalidate preview counts;
- clear the request guard;
- on any mismatch disable building for the current world.

On LoadMap pre, restore immediately instead of waiting for the timeout.

- [ ] **Step 5: Keep repair explicitly unavailable**

Publish:

```text
Palworld 1.0.1 尚未验证安全的修理材料检查与扣除入口。
```

Do not hook functions merely because their names contain `Repair`. Do not expose a clickable repair-sharing control.

- [ ] **Step 6: Run tests/build**

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditor PalworldEditorBaseResourceSharingTests
ctest --test-dir build --output-on-failure
git diff --check
```

- [ ] **Step 7: Commit**

```powershell
git add mods/PalworldEditor/inc/base_resource_sharing/resource_pool.hpp `
        mods/PalworldEditor/src/pal_base_resources.cpp `
        mods/PalworldEditor/tests/base_resource_sharing_tests.cpp
git commit -m "feat: share resources for building"
```

---

### Task 8: GUI, Config, LoadMap, and Unload Integration

**Files:**
- Modify: `mods/PalworldEditor/src/dllmain.cpp`
- Modify: `mods/PalworldEditor/tests/base_resource_sharing_tests.cpp`

**Interfaces:**
- Consumes: Tasks 1–7.
- Produces: user-facing toggle/status and complete lifecycle ownership.

- [ ] **Step 1: Add a failing GUI-status formatting test**

Declare a pure formatter in `resource_pool.hpp`:

```cpp
struct BaseResourceSharingStatus {
    bool enabled{};
    std::size_t baseCount{};
    std::size_t containerCount{};
    bool craftingAvailable{};
    bool buildingAvailable{};
    bool repairAvailable{};
    std::string repairError;
};

auto format_status(const BaseResourceSharingStatus& status) -> std::string;

void test_status_text_reports_partial_support() {
    using namespace base_resource_sharing;
    BaseResourceSharingStatus status{
        .enabled = true,
        .baseCount = 3,
        .containerCount = 12,
        .craftingAvailable = true,
        .buildingAvailable = true,
        .repairAvailable = false,
        .repairError = "Palworld 1.0.1 尚未验证安全的修理材料检查与扣除入口。",
    };
    const auto text = format_status(status);
    CHECK(text.find("3 个据点") != std::string::npos);
    CHECK(text.find("12 个资源容器") != std::string::npos);
    CHECK(text.find("制作：可用") != std::string::npos);
    CHECK(text.find("建造：可用") != std::string::npos);
    CHECK(text.find("修理：不可用") != std::string::npos);
}
```

- [ ] **Step 2: Run tests and verify RED, then implement the formatter**

Return deterministic UTF-8 text; no ImGui or Unreal dependency.

- [ ] **Step 3: Load configuration and create request handoff**

In `PalworldEditorMod`:

- compute the path with
  `std::filesystem::path{UE4SSProgram::get_program().get_mods_directory()} /
   "PalworldEditor" / "config.ini"`;
- load settings in `on_program_start()` or the first safe non-Unreal lifecycle callback;
- keep GUI-to-game-thread changes as atomics:
  `requestedBaseSharingEnabled_` and `baseSharingSettingDirty_`;
- the EngineTick consumes the latest requested value and calls
  `baseResourceBridge_.set_enabled(...)`;
- persist after the GUI changes the checkbox; a save error is shown in the snapshot and does not silently claim persistence.

- [ ] **Step 4: Integrate lifecycle**

In `on_unreal_init`, continue registering only EngineTick/LoadMap callbacks.

In `game_thread_tick()`:

1. return while the world is inaccessible;
2. consume toggle changes;
3. call `baseResourceBridge_.ensure_hooks_registered()`;
4. call `baseResourceBridge_.tick(deltaSeconds)`.

Change the existing EngineTick lambda to forward its `deltaSeconds`.

In `begin_world_transition()`:

1. call `baseResourceBridge_.on_world_begin(worldSession_.generation() + 1)` while the old world is still accessible;
2. then call the existing `worldSession_.begin_transition()`;
3. keep the saved preference but publish inaccessible status.

In `finish_world_transition()`:

1. complete the existing world session transition;
2. call `baseResourceBridge_.on_world_ready(worldSession_.generation())`;
3. require capability rediscovery before any union.

In the destructor:

1. call `baseResourceBridge_.shutdown_hooks()`;
2. then unregister global EngineTick/LoadMap callbacks.

- [ ] **Step 5: Add the ImGui section**

Render a new collapsed section before the Pal editor:

```cpp
if (ImGui::CollapsingHeader("据点资源共享")) {
    bool enabled = snapshot.enabled;
    if (ImGui::Checkbox("同公会跨据点资源共享", &enabled)) {
        self->requestedBaseSharingEnabled_.store(enabled);
        self->baseSharingSettingDirty_.store(true);
    }
    ImGui::TextWrapped("%s", snapshot.status.c_str());
    if (!snapshot.configError.empty()) {
        ImGui::TextColored(ImVec4(1.0F, 0.35F, 0.2F, 1.0F),
                           "%s", snapshot.configError.c_str());
    }
}
```

Status must distinguish:

- disabled;
- waiting for a world;
- capability detection;
- crafting/building available;
- repair unavailable;
- exact restoration/capability errors.

- [ ] **Step 6: Run all automated verification**

```powershell
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor PalworldEditorTests PalworldEditorBaseResourceSharingTests
ctest --test-dir build --output-on-failure
git diff --check
```

Expected: format check succeeds, DLL links, both CTest cases pass, diff check is silent.

- [ ] **Step 7: Commit**

```powershell
git add mods/PalworldEditor/src/dllmain.cpp `
        mods/PalworldEditor/inc/base_resource_sharing/resource_pool.hpp `
        mods/PalworldEditor/tests/base_resource_sharing_tests.cpp
git commit -m "feat: add base resource sharing switch"
```

---

### Task 9: Release Documentation and Game Validation

**Files:**
- Modify: `mods/PalworldEditor/src/dllmain.cpp`
- Modify: `README.md`
- Modify: `AGENTS.md`

**Interfaces:**
- Consumes: completed build/craft implementation and verification results.
- Produces: PalworldEditor 1.5.0 documentation and a game-test handoff.

- [ ] **Step 1: Update version and user documentation**

Change all runtime/UI/log version strings from `1.4.6` to `1.5.0`.

README requirements:

- switch defaults off and persists;
- only loaded normal storage containers from the local player's guild are included;
- crafting and building are supported;
- repair displays unsupported until a safe Palworld 1.0.1 hook pair is known;
- no item movement or global box UI;
- standalone/listen-host only;
- do not run simultaneously with UBIM Lite or another mod replacing crafting/building storage request logic;
- LoadMap restores/clears active unions;
- exact config path and troubleshooting messages.

AGENTS requirements:

- add new component files to the architecture list;
- describe request-local container union and restoration;
- state that Hook callbacks and array mutation are game-thread-only;
- add the game validation cases below.

- [ ] **Step 2: Run final automated verification from a VS x64 environment**

```powershell
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor PalworldEditorTests PalworldEditorBaseResourceSharingTests
ctest --test-dir build --output-on-failure
git diff --check
rg -n "1\.4\.6" README.md AGENTS.md mods/PalworldEditor
```

Expected:

- build and format check succeed;
- two CTest tests pass;
- `git diff --check` is silent;
- `rg` returns no stale version string.

- [ ] **Step 3: Commit the release metadata**

```powershell
git add README.md AGENTS.md mods/PalworldEditor/src/dllmain.cpp
git commit -m "docs: release PalworldEditor 1.5.0"
```

- [ ] **Step 4: Build the handoff DLL without deploying**

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditor
```

Expected artifact:

```text
build/Game__Shipping__Win64/bin/PalworldEditor.dll
```

- [ ] **Step 5: Perform the authorized game validation when the user is ready**

Do not deploy unless separately authorized. After the user installs the DLL, validate:

1. Start with the switch off; crafting/building remain vanilla.
2. Turn it on; GUI reports the exact number of loaded guild bases/containers.
3. Put all material in base B and initiate crafting in base A; UI count and craftability are correct, the item is produced, and only real material is consumed.
4. Split one recipe across A/B; crafting succeeds once and total deduction is exact.
5. Repeat for construction using material only in B and then split across A/B.
6. With insufficient total material, both operations remain unavailable and consume nothing.
7. Verify physical box contents never move before a request and box UI remains local.
8. Verify food boxes, Pal transport, and automatic base production remain local.
9. Turn the switch off and confirm immediate vanilla behavior.
10. Exit/re-enter the save with the switch persisted; capability/container discovery runs again.
11. Trigger LoadMap shortly after a build request and confirm restoration, no delayed consumption, and no crash.
12. Confirm repair is visibly unavailable rather than silently using another base.
13. Inspect UE4SS logs for union-open/restore pairs and zero restoration mismatch errors.

- [ ] **Step 6: Inspect final branch state**

```powershell
git status --short
git log -10 --oneline
git diff --stat 53da3cf..HEAD
```

Expected: clean worktree and a reviewable sequence of focused commits.
