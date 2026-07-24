# Next Summon Pal Target Resolution Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Resolve the local party Pal that is currently highlighted and will be summoned by the next E press, without selecting wild or already-present scene Pals.

**Architecture:** Replace the unstable `PlayerController -> PalUtility -> Holder` lookup with a unique-local-Holder resolver. The resolver validates each `PalOtomoHolderComponentBase` through its owner pawn and local controller, then invokes `GetSelectedOtomoID` from the Holder instance's runtime class chain so the Blueprint implementation runs. Existing GUID/generation guards remain responsible for invalidating stale edits.

**Tech Stack:** C++23, UE4SS experimental runtime, Unreal reflection through `ProcessEvent`, CMake ≥ 3.22, Ninja, MSVC, standalone CTest executable.

## Global Constraints

- Target runtime is Palworld 1.0 with UE4SS Experimental and PalSchema.
- All Unreal reflection calls execute only on the `on_update()` game thread.
- No Unreal object pointer may be cached across frames or transferred to the GUI thread.
- The target is the local party's highlighted Pal that the next E press will summon.
- Wild, enemy, base-worker, and scene-scanned Pals are never fallback candidates.
- Multiple local Holder candidates are an error; never guess by taking the first one.
- Keep the existing `FPalInstanceID.InstanceId` plus generation validation before every skill write.
- Use C++23, the repository's Allman formatting, UTF-8 source, and LF line endings.
- Do not hook keyboard input or hard-code a particular numeric key.

## File Map

- `mods/PalworldEditor/inc/skills/selected_target_state.hpp`: pure C++ unique-local-candidate policy, resolution statuses, and user-facing status messages.
- `mods/PalworldEditor/tests/skill_editor_tests.cpp`: deterministic regression tests for candidate selection, ambiguity, diagnostics, GUID changes, and stale requests.
- `mods/PalworldEditor/inc/game/pal_game.hpp`: UE4SS Holder discovery, owner/controller validation, runtime Blueprint function lookup, selected party-slot resolution, and diagnostics.
- `mods/PalworldEditor/src/dllmain.cpp`: status logging, UI wording, and mod version.
- `mods/PalworldEditor/src/pal_skills.cpp`: skill-catalog world-context error wording.
- `README.md`, `AGENTS.md`, `CLAUDE.md`: numeric-key/E target semantics and versioned verification instructions.

---

### Task 1: Unique Local Holder Selection Policy

**Files:**
- Modify: `mods/PalworldEditor/inc/skills/selected_target_state.hpp:60-151`
- Test: `mods/PalworldEditor/tests/skill_editor_tests.cpp:385-475`

**Interfaces:**
- Produces: `skill_editor::LocalCandidateSelectionStatus`
- Produces: `skill_editor::LocalCandidateSelection<T>`
- Produces: `skill_editor::find_unique_local_candidate(range, isValid, ownerPawn, controller, isLocal)`
- Produces: expanded `skill_editor::SelectedTargetResolutionStatus`
- Consumes: only standard-library ranges, optionals, predicates, and value types; no Unreal headers.

- [ ] **Step 1: Replace the old first-match regression with failing unique-selection tests**

In `mods/PalworldEditor/tests/skill_editor_tests.cpp`, replace
`test_first_valid_local_candidate_is_selected()` with:

```cpp
struct LocalCandidateProbe {
    bool valid{true};
    bool hasOwnerPawn{true};
    bool hasController{true};
    bool local{};
};

auto select_local_candidate(const std::vector<LocalCandidateProbe*>& candidates) {
    return skill_editor::find_unique_local_candidate(
        candidates,
        [](const LocalCandidateProbe* candidate) { return candidate != nullptr && candidate->valid; },
        [](LocalCandidateProbe* candidate) {
            return candidate->hasOwnerPawn ? candidate : nullptr;
        },
        [](LocalCandidateProbe* pawn) { return pawn->hasController ? pawn : nullptr; },
        [](const LocalCandidateProbe* controller) { return controller->local; });
}

void test_unique_local_candidate_is_selected() {
    LocalCandidateProbe remote{.local = false};
    LocalCandidateProbe local{.local = true};
    const auto selection = select_local_candidate({&remote, &local});

    CHECK(selection.status == skill_editor::LocalCandidateSelectionStatus::success);
    CHECK(selection.candidate.has_value());
    CHECK(*selection.candidate == &local);
    CHECK(selection.candidateCount == 2);
    CHECK(selection.localCandidateCount == 1);
}

void test_local_candidate_selection_reports_each_unavailable_stage() {
    CHECK(select_local_candidate({}).status ==
          skill_editor::LocalCandidateSelectionStatus::noCandidates);

    LocalCandidateProbe noPawn{.hasOwnerPawn = false};
    CHECK(select_local_candidate({&noPawn}).status ==
          skill_editor::LocalCandidateSelectionStatus::ownerPawnUnavailable);

    LocalCandidateProbe noController{.hasController = false};
    CHECK(select_local_candidate({&noController}).status ==
          skill_editor::LocalCandidateSelectionStatus::ownerControllerUnavailable);

    LocalCandidateProbe remote{.local = false};
    CHECK(select_local_candidate({&remote}).status ==
          skill_editor::LocalCandidateSelectionStatus::localCandidateUnavailable);
}

void test_multiple_local_candidates_are_rejected() {
    LocalCandidateProbe first{.local = true};
    LocalCandidateProbe second{.local = true};
    const auto selection = select_local_candidate({&first, &second});

    CHECK(selection.status ==
          skill_editor::LocalCandidateSelectionStatus::ambiguousLocalCandidates);
    CHECK(!selection.candidate.has_value());
    CHECK(selection.localCandidateCount == 2);
}
```

Replace the old call in `main()` with:

```cpp
test_unique_local_candidate_is_selected();
test_local_candidate_selection_reports_each_unavailable_stage();
test_multiple_local_candidates_are_rejected();
```

- [ ] **Step 2: Run the tests and verify RED**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorTests
```

Expected: compilation fails because `LocalCandidateSelectionStatus` and
`find_unique_local_candidate` do not exist.

- [ ] **Step 3: Add the pure unique-candidate policy**

In `mods/PalworldEditor/inc/skills/selected_target_state.hpp`, add `<cstddef>` and replace
`find_local_candidate` with:

```cpp
enum class LocalCandidateSelectionStatus {
    success,
    noCandidates,
    ownerPawnUnavailable,
    ownerControllerUnavailable,
    localCandidateUnavailable,
    ambiguousLocalCandidates,
};

template <typename Candidate>
struct LocalCandidateSelection {
    std::optional<Candidate> candidate;
    LocalCandidateSelectionStatus status{LocalCandidateSelectionStatus::noCandidates};
    std::size_t candidateCount{};
    std::size_t localCandidateCount{};
};

template <std::ranges::input_range Range, typename IsValid, typename ResolveOwnerPawn,
          typename ResolveController, typename IsLocal>
[[nodiscard]] auto find_unique_local_candidate(const Range& candidates, IsValid&& isValid,
                                               ResolveOwnerPawn&& resolveOwnerPawn,
                                               ResolveController&& resolveController,
                                               IsLocal&& isLocal)
    -> LocalCandidateSelection<std::ranges::range_value_t<Range>> {
    using Candidate = std::ranges::range_value_t<Range>;

    LocalCandidateSelection<Candidate> result;
    bool foundOwnerPawn{};
    bool foundController{};

    for (const auto& candidate : candidates) {
        if (!std::invoke(isValid, candidate)) {
            continue;
        }

        ++result.candidateCount;
        const auto ownerPawn = std::invoke(resolveOwnerPawn, candidate);
        if (!ownerPawn) {
            continue;
        }
        foundOwnerPawn = true;

        const auto controller = std::invoke(resolveController, ownerPawn);
        if (!controller) {
            continue;
        }
        foundController = true;

        if (!std::invoke(isLocal, controller)) {
            continue;
        }

        ++result.localCandidateCount;
        if (result.localCandidateCount == 1) {
            result.candidate = candidate;
        } else {
            result.candidate.reset();
        }
    }

    if (result.candidateCount == 0) {
        result.status = LocalCandidateSelectionStatus::noCandidates;
    } else if (!foundOwnerPawn) {
        result.status = LocalCandidateSelectionStatus::ownerPawnUnavailable;
    } else if (!foundController) {
        result.status = LocalCandidateSelectionStatus::ownerControllerUnavailable;
    } else if (result.localCandidateCount == 0) {
        result.status = LocalCandidateSelectionStatus::localCandidateUnavailable;
    } else if (result.localCandidateCount > 1) {
        result.status = LocalCandidateSelectionStatus::ambiguousLocalCandidates;
    } else {
        result.status = LocalCandidateSelectionStatus::success;
    }

    return result;
}
```

- [ ] **Step 4: Add the new target-resolution statuses and messages**

Add these values to `SelectedTargetResolutionStatus` immediately after `success`:

```cpp
holderCandidatesUnavailable,
holderOwnerPawnUnavailable,
holderOwnerControllerUnavailable,
localHolderUnavailable,
localHolderAmbiguous,
```

Keep `worldContextUnavailable`, `palUtilityUnavailable`, `getHolderFunctionUnavailable`, and
`holderUnavailable` temporarily so the full DLL remains compilable between Tasks 1 and 2. Task 3
removes them after all call sites have migrated. Keep the later Handle, Parameter, GUID, and
CharacterID statuses, including the existing `getSelectedFunctionUnavailable` and
`selectedSlotUnavailable`, unchanged.

Replace the corresponding cases in `resolution_status_message()` with:

```cpp
case holderCandidatesUnavailable:
    return "未发现队伍 Holder";
case holderOwnerPawnUnavailable:
    return "未取得队伍 Holder 的所属玩家角色";
case holderOwnerControllerUnavailable:
    return "未取得队伍 Holder 的所属控制器";
case localHolderUnavailable:
    return "未找到本地玩家的队伍 Holder";
case localHolderAmbiguous:
    return "发现多个本地玩家队伍 Holder，已拒绝猜测";
```

Replace the existing messages for the two slot-resolution values with:

```cpp
case getSelectedFunctionUnavailable:
    return "实际 Holder 类未实现 GetSelectedOtomoID";
case selectedSlotUnavailable:
    return "当前高亮队伍槽位没有有效帕鲁";
```

Replace `test_resolution_status_has_actionable_message()` with:

```cpp
void test_resolution_status_has_actionable_message() {
    using enum skill_editor::SelectedTargetResolutionStatus;
    CHECK(skill_editor::resolution_status_message(holderCandidatesUnavailable) ==
          "未发现队伍 Holder");
    CHECK(skill_editor::resolution_status_message(holderOwnerPawnUnavailable) ==
          "未取得队伍 Holder 的所属玩家角色");
    CHECK(skill_editor::resolution_status_message(holderOwnerControllerUnavailable) ==
          "未取得队伍 Holder 的所属控制器");
    CHECK(skill_editor::resolution_status_message(localHolderUnavailable) ==
          "未找到本地玩家的队伍 Holder");
    CHECK(skill_editor::resolution_status_message(localHolderAmbiguous) ==
          "发现多个本地玩家队伍 Holder，已拒绝猜测");
    CHECK(skill_editor::resolution_status_message(getSelectedFunctionUnavailable) ==
          "实际 Holder 类未实现 GetSelectedOtomoID");
    CHECK(skill_editor::resolution_status_message(selectedSlotUnavailable) ==
          "当前高亮队伍槽位没有有效帕鲁");
    CHECK(skill_editor::resolution_status_message(success).empty());
}
```

- [ ] **Step 5: Run focused tests and verify GREEN**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorTests
ctest --test-dir build -R PalworldEditor.SkillEditor --output-on-failure
```

Expected: build succeeds and `PalworldEditor.SkillEditor` passes.

- [ ] **Step 6: Commit the pure selection policy**

```powershell
git add mods/PalworldEditor/inc/skills/selected_target_state.hpp mods/PalworldEditor/tests/skill_editor_tests.cpp
git commit -m "test: define unique local Pal holder selection"
```

---

### Task 2: UE4SS Local Holder and Blueprint Slot Resolution

**Files:**
- Modify: `mods/PalworldEditor/inc/game/pal_game.hpp:45-221`

**Interfaces:**
- Consumes: `find_unique_local_candidate(...)` from Task 1.
- Produces: `pal_game::LocalOtomoHolderResolution`
- Produces: `pal_game::resolve_local_otomo_holder()`
- Preserves: `pal_game::get_world_context() -> UObject*`
- Preserves: `pal_game::resolve_selected_otomo() -> SelectedPalTarget`

- [ ] **Step 1: Run the current full mod build as a baseline**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditor
```

Expected: the existing target builds before reflection wiring changes. If it does not, stop and
diagnose the pre-existing build failure separately.

- [ ] **Step 2: Add local Holder diagnostics and owner-chain callbacks**

In `mods/PalworldEditor/inc/game/pal_game.hpp`, add `<cstddef>` and `<utility>`, then add:

```cpp
struct LocalOtomoHolderResolution {
    UObject* holder{};
    skill_editor::SelectedTargetResolutionStatus status{
        skill_editor::SelectedTargetResolutionStatus::holderCandidatesUnavailable};
    std::size_t candidateCount{};
    std::size_t localCandidateCount{};
    std::wstring candidateClasses;
};

[[nodiscard]] inline auto invoke_object_return(UObject* object, const TCHAR* functionName)
    -> UObject* {
    if (!is_valid(object)) {
        return nullptr;
    }
    auto* const function = object->GetFunctionByNameInChain(functionName);
    if (function == nullptr) {
        return nullptr;
    }
    struct Params {
        UObject* ReturnValue{};
    } params;
    object->ProcessEvent(function, &params);
    return is_valid(params.ReturnValue) ? params.ReturnValue : nullptr;
}

[[nodiscard]] inline auto invoke_bool_return(UObject* object, const TCHAR* functionName) -> bool {
    if (!is_valid(object)) {
        return false;
    }
    auto* const function = object->GetFunctionByNameInChain(functionName);
    if (function == nullptr) {
        return false;
    }
    struct Params {
        bool ReturnValue{};
    } params;
    object->ProcessEvent(function, &params);
    return params.ReturnValue;
}
```

Also change the default `SelectedPalTarget::status` initializer to:

```cpp
skill_editor::SelectedTargetResolutionStatus::holderCandidatesUnavailable
```

Then implement:

```cpp
[[nodiscard]] inline auto resolve_local_otomo_holder() -> LocalOtomoHolderResolution {
    std::vector<UObject*> holders;
    UObjectGlobals::FindAllOf(STR("PalOtomoHolderComponentBase"), holders);

    std::wstring candidateClasses;
    for (auto* const holder : holders) {
        if (!is_valid(holder)) {
            continue;
        }
        if (!candidateClasses.empty()) {
            candidateClasses.append(STR(", "));
        }
        candidateClasses.append(holder->GetClassPrivate()->GetName());
    }

    const auto selection = skill_editor::find_unique_local_candidate(
        holders, [](UObject* holder) { return is_valid(holder); },
        [](UObject* holder) {
            return invoke_object_return(holder, STR("TryGetOwnerControlledPawn"));
        },
        [](UObject* pawn) { return invoke_object_return(pawn, STR("GetController")); },
        [](UObject* controller) {
            return invoke_bool_return(controller, STR("IsLocalPlayerController"));
        });

    using SelectionStatus = skill_editor::LocalCandidateSelectionStatus;
    using ResolutionStatus = skill_editor::SelectedTargetResolutionStatus;
    const auto status = [&] {
        switch (selection.status) {
            case SelectionStatus::success:
                return ResolutionStatus::success;
            case SelectionStatus::noCandidates:
                return ResolutionStatus::holderCandidatesUnavailable;
            case SelectionStatus::ownerPawnUnavailable:
                return ResolutionStatus::holderOwnerPawnUnavailable;
            case SelectionStatus::ownerControllerUnavailable:
                return ResolutionStatus::holderOwnerControllerUnavailable;
            case SelectionStatus::localCandidateUnavailable:
                return ResolutionStatus::localHolderUnavailable;
            case SelectionStatus::ambiguousLocalCandidates:
                return ResolutionStatus::localHolderAmbiguous;
        }
        return ResolutionStatus::localHolderUnavailable;
    }();

    return {
        .holder = selection.candidate.value_or(nullptr),
        .status = status,
        .candidateCount = selection.candidateCount,
        .localCandidateCount = selection.localCandidateCount,
        .candidateClasses = std::move(candidateClasses),
    };
}
```

- [ ] **Step 3: Make the shared world context use the same Holder resolver**

Replace `get_world_context()` with:

```cpp
[[nodiscard]] inline auto get_world_context() -> UObject* {
    return resolve_local_otomo_holder().holder;
}
```

This keeps `pal_skills.cpp` source-compatible while eliminating its independent
`PlayerController` discovery.

- [ ] **Step 4: Resolve the selected slot from the Holder runtime class**

Add these fields to `SelectedPalTarget`:

```cpp
std::size_t holderCandidateCount{};
std::size_t localHolderCandidateCount{};
std::wstring holderCandidateClasses;
```

At the start of `resolve_selected_otomo()`, replace the world-context, `PalUtility`, and
`GetOtomoHolderComponent` block with:

```cpp
using enum skill_editor::SelectedTargetResolutionStatus;
auto holderResolution = resolve_local_otomo_holder();
auto failure = [&holderResolution](const skill_editor::SelectedTargetResolutionStatus status) {
    return SelectedPalTarget{
        .status = status,
        .holderCandidateCount = holderResolution.candidateCount,
        .localHolderCandidateCount = holderResolution.localCandidateCount,
        .holderCandidateClasses = std::move(holderResolution.candidateClasses),
    };
};

auto* const holder = holderResolution.holder;
if (!is_valid(holder)) {
    return failure(holderResolution.status);
}

auto* const getSelectedFunction =
    holder->GetFunctionByNameInChain(STR("GetSelectedOtomoID"));
if (getSelectedFunction == nullptr) {
    return failure(getSelectedFunctionUnavailable);
}
struct GetSelectedParams {
    int32_t ReturnValue{-1};
} getSelectedParams;
holder->ProcessEvent(getSelectedFunction, &getSelectedParams);
if (getSelectedParams.ReturnValue < 0) {
    return failure(selectedSlotUnavailable);
}
```

For every later failure return in this function, replace
`return {.status = someStatus};` with `return failure(someStatus);`.

In the success return, append:

```cpp
.holderCandidateCount = holderResolution.candidateCount,
.localHolderCandidateCount = holderResolution.localCandidateCount,
.holderCandidateClasses = std::move(holderResolution.candidateClasses),
```

Do not change `GetOtomoIndividualHandle`, `TryGetIndividualParameter`, `GetPalId`, or
`GetCharacterID` parameter layouts.

Update the `pal_game.hpp` comments for `SelectedPalTarget` and `resolve_selected_otomo()` to say
“数字键当前高亮、下一次按 E 召唤的队伍帕鲁”; remove their obsolete Q/E wording.

- [ ] **Step 5: Build and run the standalone regressions**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditor PalworldEditorTests
ctest --test-dir build --output-on-failure
```

Expected: both targets build and all CTest tests pass.

- [ ] **Step 6: Commit the UE4SS resolver**

```powershell
git add mods/PalworldEditor/inc/game/pal_game.hpp
git commit -m "fix: resolve next summon Pal from local holder"
```

---

### Task 3: Diagnostics, User Wording, Version, and Documentation

**Files:**
- Modify: `mods/PalworldEditor/src/dllmain.cpp:45-60,157-169,630-765`
- Modify: `mods/PalworldEditor/src/pal_skills.cpp:285-294`
- Modify: `mods/PalworldEditor/inc/skills/selected_target_state.hpp`
- Modify: `README.md`
- Modify: `AGENTS.md`
- Modify: `CLAUDE.md`

**Interfaces:**
- Consumes: Holder diagnostic fields from Task 2.
- Produces: status-change logs with candidate counts and runtime class names.
- Produces: UI and documentation that describe numeric-key selection and next-E summon semantics.
- Produces: version `1.4.3`.

- [ ] **Step 1: Establish RED wording scans**

Run:

```powershell
rg -n "Q/E|1\.4\.2|Local PlayerController world context" README.md AGENTS.md CLAUDE.md mods/PalworldEditor/src mods/PalworldEditor/inc
```

Expected: matches appear in runtime UI, diagnostics, metadata, and user documentation.

- [ ] **Step 2: Update status-change logging**

Replace the current status-only log in `dllmain.cpp` with:

```cpp
if (!lastResolutionStatus_.has_value() || *lastResolutionStatus_ != selectedPal.status) {
    Output::send<LogLevel::Warning>(
        STR("PalworldEditor: selected Pal resolution status={}, holder_candidates={}, "
            "local_candidates={}, classes=[{}]\n"),
        static_cast<int32>(selectedPal.status),
        static_cast<int32>(selectedPal.holderCandidateCount),
        static_cast<int32>(selectedPal.localHolderCandidateCount),
        selectedPal.holderCandidateClasses);
    lastResolutionStatus_ = selectedPal.status;
}
```

Keep the existing “log only when status changes” condition so `on_update()` does not flood the
UE4SS console.

- [ ] **Step 3: Update runtime wording and version**

In `dllmain.cpp`, replace every `1.4.2` with `1.4.3`.

Replace the disabled instruction with:

```cpp
ImGui::TextDisabled(
    "请用数字键高亮队伍帕鲁，再点击“选择当前帕鲁”；目标应与下一次按 E 召唤一致。");
```

Replace comments that say `Q/E` in `dllmain.cpp` and `selected_target_state.hpp` with
“数字键当前高亮、下一次按 E 召唤的队伍帕鲁”.

Change `SkillEditorSnapshot::resolutionStatus` to:

```cpp
skill_editor::SelectedTargetResolutionStatus resolutionStatus{
    skill_editor::SelectedTargetResolutionStatus::holderCandidatesUnavailable};
```

In `pal_skills.cpp`, replace:

```cpp
catalog.error = "Local PlayerController world context is unavailable";
```

with:

```cpp
catalog.error = "Local player party Holder world context is unavailable";
```

After the snapshot initializer is migrated, remove the obsolete
`worldContextUnavailable`, `palUtilityUnavailable`, `getHolderFunctionUnavailable`, and
`holderUnavailable` values and their switch cases from
`mods/PalworldEditor/inc/skills/selected_target_state.hpp`.

- [ ] **Step 4: Update repository documentation**

Apply the following exact semantic replacements throughout `README.md`, `AGENTS.md`, and
`CLAUDE.md`:

```text
Q/E 当前选中的下一只待出战帕鲁
```

becomes:

```text
数字键当前高亮、下一次按 E 会召唤的队伍帕鲁
```

```text
Q/E 切换
```

becomes:

```text
数字键切换队伍高亮目标
```

Update all version strings and expected load logs from `1.4.2` to `1.4.3`.
In the manual verification sections, add the wild-Pal regression:

```text
场景中保留一只野生帕鲁，队伍 UI 高亮另一只队伍帕鲁；点击“选择当前帕鲁”后，
确认目标是下一次按 E 会召唤的队伍帕鲁，而不是场景中的野生帕鲁。
```

- [ ] **Step 5: Verify wording scans are GREEN**

Run:

```powershell
rg -n "Q/E|1\.4\.2|Local PlayerController world context" README.md AGENTS.md CLAUDE.md mods/PalworldEditor/src mods/PalworldEditor/inc
```

Expected: no matches.

Run:

```powershell
rg -n "1\.4\.3|下一次按 E|数字键" README.md AGENTS.md CLAUDE.md mods/PalworldEditor/src
```

Expected: matches appear in metadata, GUI guidance, and all three documentation files.

- [ ] **Step 6: Run formatting and complete verification**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor PalworldEditorTests
ctest --test-dir build --output-on-failure
git diff --check
```

Expected: every build target succeeds, all tests pass, and `git diff --check` prints nothing.

- [ ] **Step 7: Commit runtime messages and documentation**

```powershell
git add mods/PalworldEditor/src/dllmain.cpp mods/PalworldEditor/src/pal_skills.cpp mods/PalworldEditor/inc/skills/selected_target_state.hpp README.md AGENTS.md CLAUDE.md
git commit -m "docs: describe next summon Pal targeting"
```

---

### Task 4: Game Runtime Verification and Deployment Handoff

**Files:**
- No source changes expected.

**Interfaces:**
- Consumes: built `build/Game__Shipping__Win64/bin/PalworldEditor.dll`.
- Produces: game-runtime evidence that the reflected target equals the next E summon target.

- [ ] **Step 1: Build the deployable DLL**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditor
```

Expected: `build/Game__Shipping__Win64/bin/PalworldEditor.dll` exists.

- [ ] **Step 2: Deploy only when `PALWORLD_INSTALL_DIR` is already configured**

Read the environment without changing it:

```powershell
Get-Item Env:PALWORLD_INSTALL_DIR
```

If it exists and points to the user's Palworld installation, run:

```powershell
cmake --build --preset ninja-msvc-x64 --target deploy
```

Expected: the deploy target copies the DLL as
`Pal/Binaries/Win64/ue4ss/Mods/PalworldEditor/dlls/main.dll` and preserves/enables
`enabled.txt`.

If the variable is absent, do not guess an installation path; report the built DLL path for manual
deployment.

- [ ] **Step 3: Give the exact in-game verification checklist**

Verify:

1. UE4SS logs `PalworldEditor loaded (v1.4.3)`.
2. Put Rushoar/草莽猪 and at least one other Pal in the local party.
3. Leave a wild Lamball/棉悠悠 present in the scene.
4. Use numeric selection to highlight 草莽猪 without pressing E.
5. Click “选择当前帕鲁”; the editor reports 草莽猪, never the wild 棉悠悠.
6. Press E; the summoned party Pal matches the editor target.
7. Change the highlighted party Pal; the editor invalidates the prior target.
8. Confirm logs no longer alternate through the old `1/4/6` statuses. On failure, record the new
   status, Holder candidate counts, and runtime class list.

- [ ] **Step 4: Record final repository state**

Run:

```powershell
git status --short
git log -4 --oneline
```

Expected: no uncommitted implementation changes; recent commits include the design, pure selection
policy, resolver, and documentation commits.
