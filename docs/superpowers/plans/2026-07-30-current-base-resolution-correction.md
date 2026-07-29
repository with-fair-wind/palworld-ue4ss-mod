# Current Base Resolution Correction Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the broken nearest-base safety gate with Palworld's native inside-base component so the first crafting or building menu can establish a correct, consumable shared-resource union.

**Architecture:** A small Unreal-independent contract header owns the reflected route names and the pure acceptance rule. The UE4SS runtime follows that contract on the game thread (`controller -> K2_GetPawn -> InsideBaseCampCheckComponent -> GetInsideBaseCampModel -> GetId`), then the foreground session layer records the observed base and rejects stale unions at submit time.

**Tech Stack:** C++23, UE4SS Experimental, Palworld 1.0.1 reflected `UFunction`/`FProperty` access, CMake, Ninja, MSVC, CTest.

## Global Constraints

- Support PalworldEditor 1.6.9, Palworld 1.0.1, and UE4SS Experimental.
- Do not add EngineTick work, background threads, timers, global UObject scans, or new hooks.
- Resolve the current base only when opening a new crafting/building session and immediately before a real submit.
- Do not fall back to `PalBaseCampManager:GetNearestBaseCamp`.
- On any reflection, membership, or storage-module failure, restore the active union and keep the operation on original game behavior.
- Only store GUIDs, object full names, and standard-library ledger values across frames; never retain Unreal object pointers.
- Keep crafting and building mutually exclusive and preserve the single-consumer-surface rule.
- Build and test in an x64 Visual Studio 2022 developer environment.

---

### Task 1: Add a testable native current-base contract

**Files:**
- Create: `mods/PalworldEditor/inc/base_resource_sharing/current_base_resolution.hpp`
- Modify: `mods/PalworldEditor/tests/base_resource_sharing_tests.cpp`

**Interfaces:**
- Produces: `template <typename Character> struct CurrentBaseReflectionNames`
- Produces: `inline constexpr bool kAllowsNearestBaseFallback`
- Produces: `accept_current_base(GuidKey, bool) -> std::optional<GuidKey>`
- Consumes: `base_resource_sharing::GuidKey` from `resource_pool.hpp`

- [ ] **Step 1: Write the failing contract tests**

Add the header include and this test:

```cpp
#include <base_resource_sharing/current_base_resolution.hpp>

void test_current_base_resolution_uses_native_inside_base_route() {
    using namespace base_resource_sharing;
    using Names = CurrentBaseReflectionNames<char>;

    CHECK(Names::controllerPawnFunction == "K2_GetPawn");
    CHECK(Names::insideComponentProperty == "InsideBaseCampCheckComponent");
    CHECK(Names::insideBaseModelFunction == "GetInsideBaseCampModel");
    CHECK(Names::baseIdFunction == "GetId");
    CHECK(!kAllowsNearestBaseFallback);

    const GuidKey current{{7, 0, 0, 0}};
    CHECK(accept_current_base(current, true) == current);
    CHECK(!accept_current_base(current, false).has_value());
    CHECK(!accept_current_base(GuidKey{}, true).has_value());
}
```

Register `test_current_base_resolution_uses_native_inside_base_route()` in the test executable's existing `main()`.

- [ ] **Step 2: Run the resource-sharing test target and verify the new test fails**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorBaseResourceSharingTests
```

Expected: compilation fails because `current_base_resolution.hpp` and its contract symbols do not exist.

- [ ] **Step 3: Implement the minimal pure contract**

Create the header with narrow and wide compile-time names so production code and tests consume one source of truth:

```cpp
#pragma once

#include <optional>
#include <string_view>

#include <base_resource_sharing/resource_pool.hpp>

namespace base_resource_sharing {
template <typename Character>
struct CurrentBaseReflectionNames;

template <>
struct CurrentBaseReflectionNames<char> {
    static constexpr std::string_view controllerPawnFunction{"K2_GetPawn"};
    static constexpr std::string_view insideComponentProperty{"InsideBaseCampCheckComponent"};
    static constexpr std::string_view insideBaseModelFunction{"GetInsideBaseCampModel"};
    static constexpr std::string_view baseIdFunction{"GetId"};
};

template <>
struct CurrentBaseReflectionNames<wchar_t> {
    static constexpr std::wstring_view controllerPawnFunction{L"K2_GetPawn"};
    static constexpr std::wstring_view insideComponentProperty{L"InsideBaseCampCheckComponent"};
    static constexpr std::wstring_view insideBaseModelFunction{L"GetInsideBaseCampModel"};
    static constexpr std::wstring_view baseIdFunction{L"GetId"};
};

inline constexpr bool kAllowsNearestBaseFallback = false;

[[nodiscard]] constexpr auto accept_current_base(const GuidKey candidate,
                                                 const bool hasStorageModule) noexcept
    -> std::optional<GuidKey> {
    return candidate.valid() && hasStorageModule ? std::optional{candidate} : std::nullopt;
}
}  // namespace base_resource_sharing
```

- [ ] **Step 4: Run the resource-sharing tests**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorBaseResourceSharingTests
ctest --test-dir build -R PalworldEditor.BaseResourceSharing --output-on-failure
```

Expected: build succeeds and the resource-sharing test passes.

- [ ] **Step 5: Commit the contract and test**

```powershell
git add mods/PalworldEditor/inc/base_resource_sharing/current_base_resolution.hpp mods/PalworldEditor/tests/base_resource_sharing_tests.cpp
git commit -m "test: define native current base resolution contract"
```

### Task 2: Replace nearest-base inference with the inside-base component

**Files:**
- Modify: `mods/PalworldEditor/src/pal_base_resource_runtime.hpp:59-61`
- Modify: `mods/PalworldEditor/src/pal_base_resource_runtime.cpp:129-145,518-612`

**Interfaces:**
- Consumes: `CurrentBaseReflectionNames<RC::Unreal::CharType>`
- Consumes: `accept_current_base(GuidKey, bool) -> std::optional<GuidKey>`
- Produces: `resolve_inside_base_id(UObject*, const ResourceCatalogSnapshot&, std::string&) -> std::optional<GuidKey>`

- [ ] **Step 1: Rename the runtime declaration and add the contract include**

In `pal_base_resource_runtime.hpp`, replace `resolve_nearest_base_id` with:

```cpp
[[nodiscard]] auto resolve_inside_base_id(RC::Unreal::UObject* worldContext,
                                          const ResourceCatalogSnapshot& catalog,
                                          std::string& error) -> std::optional<GuidKey>;
```

In `pal_base_resource_runtime.cpp`, include:

```cpp
#include <base_resource_sharing/current_base_resolution.hpp>
```

- [ ] **Step 2: Add a non-owning object-property reader**

Next to `try_get_object`, add:

```cpp
[[nodiscard]] auto read_object_property(UObject* object, const CharType* propertyName) -> UObject* {
    auto* property =
        object == nullptr
            ? nullptr
            : CastField<FObjectPropertyBase>(object->GetPropertyByNameInChain(propertyName));
    return property == nullptr
               ? nullptr
               : property->GetObjectPropertyValue(property->ContainerPtrToValuePtr<void>(object));
}
```

The returned pointer must remain local to the current hook callback.

- [ ] **Step 3: Implement the native inside-base route**

Update `read_base_id` to consume the shared `baseIdFunction` contract and the existing
GUID-returning function helper:

```cpp
using Names = CurrentBaseReflectionNames<CharType>;
FGuid value{};
if (!try_get_guid(baseModel, Names::baseIdFunction.data(), value)) {
    return std::nullopt;
}
```

Replace the entire location/manager/nearest-base implementation with:

```cpp
auto resolve_inside_base_id(UObject* worldContext, const ResourceCatalogSnapshot& catalog,
                            std::string& error) -> std::optional<GuidKey> {
    using Names = CurrentBaseReflectionNames<CharType>;
    error.clear();

    UObject* controller{};
    if (!call_utility_object(worldContext, STR("GetLocalPalPlayerController"), controller)) {
        error = "无法解析本地玩家控制器。";
        return std::nullopt;
    }

    UObject* pawn{};
    if (!try_get_object(controller, Names::controllerPawnFunction.data(), pawn)) {
        error = "本地玩家控制器无法通过 K2_GetPawn 返回 Pawn。";
        return std::nullopt;
    }

    auto* insideComponent =
        read_object_property(pawn, Names::insideComponentProperty.data());
    if (insideComponent == nullptr) {
        error = "本地 Pawn 缺少 InsideBaseCampCheckComponent。";
        return std::nullopt;
    }

    UObject* baseModel{};
    if (!try_get_object(insideComponent, Names::insideBaseModelFunction.data(), baseModel)) {
        error = "当前不在游戏已确认的据点内。";
        return std::nullopt;
    }

    const auto candidate = read_base_id(baseModel);
    if (!candidate.has_value()) {
        error = "当前据点模型的 GetId 未返回有效 GUID。";
        return std::nullopt;
    }

    const bool hasStorageModule = std::ranges::any_of(
        catalog.modules, [&](const auto& module) { return module.baseId == *candidate; });
    const auto accepted = accept_current_base(*candidate, hasStorageModule);
    if (!accepted.has_value()) {
        error = "当前据点不在同公会普通仓储目录中。";
    }
    return accepted;
}
```

Delete all `K2_GetActorLocation`, `GetBaseCampManager`, `GetNearestBaseCamp`, `FVector` parameter-discovery, and nearest-base error handling from this path.

- [ ] **Step 4: Compile the mod and prove nearest-base inference is gone**

Run:

```powershell
rg -n "resolve_nearest_base_id|GetNearestBaseCamp|K2_GetActorLocation" mods/PalworldEditor/src mods/PalworldEditor/inc
cmake --build --preset ninja-msvc-x64 --target PalworldEditor
```

Expected: `rg` returns no matches in the mod source/include tree; the DLL target builds.

- [ ] **Step 5: Commit the runtime resolver**

```powershell
git add mods/PalworldEditor/src/pal_base_resource_runtime.hpp mods/PalworldEditor/src/pal_base_resource_runtime.cpp
git commit -m "fix: resolve the current base from native player state"
```

### Task 3: Wire session acquisition and submit validation to the corrected base

**Files:**
- Modify: `mods/PalworldEditor/inc/base_resource_sharing/resource_session.hpp:233-270`
- Modify: `mods/PalworldEditor/src/pal_base_resources.cpp:485-589`
- Modify: `mods/PalworldEditor/tests/base_resource_sharing_tests.cpp`

**Interfaces:**
- Consumes: `resolve_inside_base_id(...)`
- Produces: `CurrentBaseState::observe(std::optional<GuidKey>, std::uint64_t) -> bool`
- Preserves: `make_exposure_plan(...)`, `apply_union(...)`, and `validate_union(...)`

- [ ] **Step 1: Write the failing observed-base state test**

Extend the existing current-base state test:

```cpp
state.begin_world(9);
CHECK(state.observe(baseA, 9));
CHECK(state.current(9) == baseA);
CHECK(state.observe(std::nullopt, 9));
CHECK(!state.current(9).has_value());
CHECK(!state.observe(baseB, 8));
```

- [ ] **Step 2: Run the resource-sharing tests and verify failure**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorBaseResourceSharingTests
```

Expected: compilation fails because `CurrentBaseState::observe` does not exist.

- [ ] **Step 3: Implement observed-base state replacement**

Add to `CurrentBaseState`:

```cpp
[[nodiscard]] auto observe(const std::optional<GuidKey> baseId,
                           const std::uint64_t generation) noexcept -> bool {
    if (generation != generation_ || (baseId.has_value() && !baseId->valid())) {
        return false;
    }
    current_ = baseId;
    return true;
}
```

- [ ] **Step 4: Use the corrected resolver at acquisition and submit**

In both `ensure_exposure_before_original` and `validate_exposure_before_original`:

```cpp
const auto currentBase = detail::resolve_inside_base_id(context, catalog_, error);
static_cast<void>(currentBase_.observe(currentBase, generation));
```

Keep acquisition failure non-destructive: release the session, report the resolver error, and do not establish a union. Keep submit mismatch destructive to the temporary union: release the session, restore the ledger, and disable only the affected operation for the world if validation cannot prove the same base.

- [ ] **Step 5: Run focused and full tests**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorBaseResourceSharingTests PalworldEditor
ctest --test-dir build -R PalworldEditor.BaseResourceSharing --output-on-failure
```

Expected: both targets build and the focused CTest passes.

- [ ] **Step 6: Commit the session wiring**

```powershell
git add mods/PalworldEditor/inc/base_resource_sharing/resource_session.hpp mods/PalworldEditor/src/pal_base_resources.cpp mods/PalworldEditor/tests/base_resource_sharing_tests.cpp
git commit -m "fix: validate resource unions against the observed base"
```

### Task 4: Align documentation, verify, and deploy

**Files:**
- Modify: `AGENTS.md`
- Modify: `README.md`
- Verify: all changed resource-sharing implementation and tests

**Interfaces:**
- Consumes: the corrected current-base route and existing deployment target
- Produces: a deployed `main.dll` ready for the user's game test

- [ ] **Step 1: Update behavior documentation**

Document that current-base resolution uses `K2_GetPawn` and `InsideBaseCampCheckComponent:GetInsideBaseCampModel`, that it has no nearest-base fallback, and that it runs only at foreground session acquisition and real submit validation.

- [ ] **Step 2: Run static safety checks**

Run:

```powershell
rg -n 'STR\("GetPawn"\)|GetNearestBaseCamp|K2_GetActorLocation|resolve_nearest_base_id' mods/PalworldEditor/src mods/PalworldEditor/inc
git diff --check
```

Expected: no obsolete current-base route remains; `git diff --check` is clean.

- [ ] **Step 3: Run the complete verification suite**

Run in the x64 Visual Studio 2022 developer environment:

```powershell
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor PalworldEditorTests PalworldEditorBaseResourceSharingTests
ctest --test-dir build --output-on-failure
```

Expected: all build targets succeed and CTest reports 100% pass.

- [ ] **Step 4: Deploy and compare hashes**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target deploy
Get-FileHash build/Game__Shipping__Win64/bin/PalworldEditor.dll -Algorithm SHA256
Get-FileHash 'F:\Program Files (x86)\Steam\steamapps\common\Palworld\Pal\Binaries\Win64\ue4ss\Mods\PalworldEditor\dlls\main.dll' -Algorithm SHA256
```

Expected: deploy succeeds and both SHA-256 hashes are identical.

- [ ] **Step 5: Commit documentation and verification metadata**

```powershell
git add AGENTS.md README.md
git commit -m "docs: describe native current base resolution"
```

- [ ] **Step 6: Hand off the exact game test**

Ask the user to test in this order:

1. Cold-enter the world, enable resource sharing, and press B once without opening a furnace or chest.
2. Confirm the first building menu is selectable and building consumes the displayed remote material exactly once.
3. Cancel a second placement and confirm only its actually deducted material is returned.
4. Cold-enter again, open a crafting station first, and confirm maximum craft count equals the completed count.
5. Leave the base and verify both operations remain original behavior.
6. While sharing is enabled, hold right mouse, Shift, and a movement key and compare frame pacing with sharing disabled.
