# Pal Passive Skill Presets Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add two expandable C++ passive-skill presets that replace the selected Pal's four passive traits through one validated, rollback-capable request without adding steady-state game-thread work.

**Architecture:** A header-only preset catalog owns immutable preset data. The existing skill edit request gains a `replaceAllPassives` operation and desired passive list; the pure service applies a bounded set difference, rereads game state, and restores the original set on failure. ImGui only selects a preset and queues one value request; all target resolution and Unreal calls remain in the existing EngineTick request path.

**Tech Stack:** C++23, STL (`std::array`, `std::span`, `std::vector`, ranges), ImGui, UE4SS `ProcessEvent`, CMake/Ninja, the repository's standalone C++ test executable and CTest.

## Global Constraints

- Target PalworldEditor version is `1.6.4`; target game data is Palworld `1.0.1`.
- Preset names are `工作毕业1` and `工作毕业2`.
- Applying a preset produces exactly its four passive Raw IDs; existing unrelated passives are not retained.
- Selecting a dropdown entry never writes; only the separate `应用预设` button queues work.
- Presets stay compiled into C++; no external preset configuration is added.
- Do not add Hook registrations, threads, timers, idle scans, catalog refreshes, or `FindAllOf`.
- Do not cache `UObject*` or Unreal array addresses across callbacks.
- Applying an already-matching preset performs zero passive write calls.
- Use a single bounded request and difference-based writes; do not enqueue four independent edits.
- Preserve existing target/world-generation validation and LoadMap request cancellation.
- Follow test-first red-green-refactor for every production behavior.
- Do not modify or stage the paused untracked file `docs/superpowers/plans/2026-07-26-extensible-material-operation-sessions.md`.

---

## File Structure

- Create `mods/PalworldEditor/inc/skills/passive_skill_presets.hpp`: immutable preset definitions, invariant check, and request factory used by UI/tests.
- Modify `mods/PalworldEditor/inc/skills/skill_editor_service.hpp`: batch request data, difference execution, reread verification, and rollback.
- Modify `mods/PalworldEditor/tests/skill_editor_tests.cpp`: preset, request, difference, rollback, and regression tests.
- Modify `mods/PalworldEditor/src/dllmain.cpp`: preset dropdown, localized preview, apply button, GUI state reset, and version strings.
- Modify `README.md`: user-facing preset workflow and performance contract.
- Modify `AGENTS.md`: architecture, lifecycle, version, and validation contract.
- Modify `CLAUDE.md`: concise architecture and validation mirror.

### Task 1: Immutable preset catalog

**Files:**
- Create: `mods/PalworldEditor/inc/skills/passive_skill_presets.hpp`
- Modify: `mods/PalworldEditor/tests/skill_editor_tests.cpp`

**Interfaces:**
- Produces: `skill_editor::PassiveSkillPreset`.
- Produces: `skill_editor::passive_skill_presets() -> std::span<const PassiveSkillPreset>`.
- Produces: `skill_editor::passive_skill_presets_are_valid() -> bool`.
- Consumed later by: the request factory and ImGui renderer.

- [ ] **Step 1: Write failing catalog tests**

Add the header include and tests with exact values:

```cpp
#include <skills/passive_skill_presets.hpp>

void test_passive_skill_presets_have_expected_palworld_1_0_ids() {
    const auto presets = skill_editor::passive_skill_presets();
    CHECK(presets.size() == 2);
    CHECK(presets[0].displayName == "工作毕业1");
    CHECK((presets[0].passiveIds ==
           std::array<std::string_view, 4>{"WorldTree_CraftSpeed", "CraftSpeed_up3",
                                           "Vampire", "CraftSpeed_up2"}));
    CHECK(presets[1].displayName == "工作毕业2");
    CHECK((presets[1].passiveIds ==
           std::array<std::string_view, 4>{"WorldTree_CraftSpeed", "CraftSpeed_up3",
                                           "CraftSpeed_up2", "PAL_CorporateSlave"}));
}

void test_passive_skill_preset_definitions_are_valid() {
    CHECK(skill_editor::passive_skill_presets_are_valid());
}
```

Call both functions from `main()`.

- [ ] **Step 2: Run the test target and verify RED**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorTests
```

Expected: compilation fails because `skills/passive_skill_presets.hpp` and its interfaces do not exist.

- [ ] **Step 3: Implement the minimal immutable catalog**

Create a self-contained header:

```cpp
#pragma once

#include <array>
#include <ranges>
#include <span>
#include <string_view>
#include <unordered_set>

namespace skill_editor {
struct PassiveSkillPreset {
    std::string_view id;
    std::string_view displayName;
    std::array<std::string_view, 4> passiveIds;
};

inline constexpr std::array kPassiveSkillPresets{
    PassiveSkillPreset{
        .id = "work-perfect-1",
        .displayName = "工作毕业1",
        .passiveIds = {"WorldTree_CraftSpeed", "CraftSpeed_up3", "Vampire",
                       "CraftSpeed_up2"},
    },
    PassiveSkillPreset{
        .id = "work-perfect-2",
        .displayName = "工作毕业2",
        .passiveIds = {"WorldTree_CraftSpeed", "CraftSpeed_up3", "CraftSpeed_up2",
                       "PAL_CorporateSlave"},
    },
};

[[nodiscard]] constexpr auto passive_skill_presets() noexcept
    -> std::span<const PassiveSkillPreset> {
    return kPassiveSkillPresets;
}

[[nodiscard]] inline auto passive_skill_presets_are_valid() -> bool {
    std::unordered_set<std::string_view> presetIds;
    for (const auto& preset : kPassiveSkillPresets) {
        std::unordered_set<std::string_view> passiveIds;
        if (preset.id.empty() || preset.displayName.empty() || !presetIds.insert(preset.id).second) {
            return false;
        }
        if (std::ranges::any_of(preset.passiveIds, [](const auto id) { return id.empty(); })) {
            return false;
        }
        passiveIds.insert(preset.passiveIds.begin(), preset.passiveIds.end());
        if (passiveIds.size() != preset.passiveIds.size()) {
            return false;
        }
    }
    return true;
}
}  // namespace skill_editor
```

- [ ] **Step 4: Run tests and verify GREEN**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorTests
ctest --test-dir build -R PalworldEditor.SkillEditor --output-on-failure
```

Expected: build succeeds and `PalworldEditor.SkillEditor` passes.

- [ ] **Step 5: Commit the catalog**

```powershell
git add mods/PalworldEditor/inc/skills/passive_skill_presets.hpp mods/PalworldEditor/tests/skill_editor_tests.cpp
git commit -m "feat: add passive skill preset catalog"
```

### Task 2: Validated difference-based batch replacement

**Files:**
- Modify: `mods/PalworldEditor/inc/skills/skill_editor_service.hpp`
- Modify: `mods/PalworldEditor/tests/skill_editor_tests.cpp`

**Interfaces:**
- Extends: `SkillEditOperation` with `replaceAllPassives`.
- Extends: `SkillEditRequest` with `std::vector<std::string> desiredPassiveIds`.
- Produces: `detail::valid_passive_set(std::span<const std::string>) -> bool`.
- Produces: `detail::apply_passive_difference(ISkillGateway&, SkillTarget, from, to) -> void`.
- Consumes: existing `ISkillGateway::add_passive`, `remove_passive`, and `read_state`.

- [ ] **Step 1: Write failing validation, no-op, full replacement, and overlap tests**

Add a request helper:

```cpp
auto passive_set_request(std::vector<std::string> ids) -> skill_editor::SkillEditRequest {
    return {
        .target = 0x1234,
        .kind = skill_editor::SkillKind::passive,
        .operation = skill_editor::SkillEditOperation::replaceAllPassives,
        .desiredPassiveIds = std::move(ids),
    };
}
```

Add tests:

```cpp
void test_passive_set_rejects_invalid_definitions_before_writing() {
    for (auto ids : {std::vector<std::string>{},
                     std::vector<std::string>{"A", "B", "C"},
                     std::vector<std::string>{"A", "B", "C", "C"},
                     std::vector<std::string>{"A", "B", "C", ""}}) {
        FakeSkillGateway gateway;
        gateway.state.passiveIds = {"O1", "O2", "O3", "O4"};
        const auto result =
            skill_editor::execute_skill_edit(gateway, passive_set_request(std::move(ids)));
        CHECK(result.status == skill_editor::SkillEditStatus::rejected);
        CHECK(gateway.calls == std::vector<std::string>({"read"}));
    }
}

void test_matching_passive_set_is_zero_write() {
    FakeSkillGateway gateway;
    gateway.state.passiveIds = {"D", "C", "B", "A"};
    const auto result = skill_editor::execute_skill_edit(
        gateway, passive_set_request({"A", "B", "C", "D"}));
    CHECK(result.status == skill_editor::SkillEditStatus::succeeded);
    CHECK(gateway.calls == std::vector<std::string>({"read"}));
}

void test_passive_set_uses_only_required_difference_writes() {
    FakeSkillGateway gateway;
    gateway.state.passiveIds = {"A", "B", "Old1", "Old2"};
    const auto result = skill_editor::execute_skill_edit(
        gateway, passive_set_request({"A", "B", "New1", "New2"}));
    CHECK(result.status == skill_editor::SkillEditStatus::succeeded);
    CHECK(gateway.calls ==
          std::vector<std::string>({"read", "remove:Old1", "remove:Old2", "add:New1",
                                    "add:New2", "read"}));
    CHECK(skill_editor::detail::same_passives(
        result.state.passiveIds, std::vector<std::string>{"A", "B", "New1", "New2"}));
}
```

Also cover a completely different original list and call all new tests from `main()`.

- [ ] **Step 2: Run the test target and verify RED**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorTests
```

Expected: compilation fails because `replaceAllPassives` and `desiredPassiveIds` do not exist.

- [ ] **Step 3: Add request fields and minimal difference execution**

Extend the request types:

```cpp
enum class SkillEditOperation {
    add,
    replace,
    remove,
    replaceAllPassives,
};

struct SkillEditRequest {
    // existing fields...
    std::vector<std::string> desiredPassiveIds;
};
```

Add pure helpers:

```cpp
[[nodiscard]] inline auto valid_passive_set(const std::span<const std::string> ids) -> bool {
    if (ids.size() != 4 ||
        std::ranges::any_of(ids, [](const auto& id) { return id.empty(); })) {
        return false;
    }
    return std::unordered_set<std::string>(ids.begin(), ids.end()).size() == ids.size();
}

[[nodiscard]] inline auto contains_passive(const std::span<const std::string> passives,
                                           const std::string_view id) -> bool {
    return std::ranges::find(passives, id) != passives.end();
}

inline auto apply_passive_difference(ISkillGateway& gateway, const SkillTarget target,
                                     const std::span<const std::string> from,
                                     const std::span<const std::string> to) -> void {
    for (const auto& id : from) {
        if (!contains_passive(to, id)) {
            static_cast<void>(gateway.remove_passive(target, id));
        }
    }
    for (const auto& id : to) {
        if (!contains_passive(from, id)) {
            static_cast<void>(gateway.add_passive(target, id));
        }
    }
}
```

Retain the existing vector overload as a forwarding wrapper so current single-skill call sites do
not change:

```cpp
[[nodiscard]] inline auto contains_passive(const std::vector<std::string>& passives,
                                           const std::string_view id) -> bool {
    return contains_passive(std::span<const std::string>{passives}, id);
}
```

Dispatch the new operation before existing add/replace/remove handling:

```cpp
if (request.operation == SkillEditOperation::replaceAllPassives) {
    if (!valid_passive_set(request.desiredPassiveIds)) {
        return reject("Passive preset must contain four unique non-empty skills");
    }
    if (same_passives(original.passiveIds, request.desiredPassiveIds)) {
        return result(SkillEditStatus::succeeded, original, "Passive preset already applied");
    }
    apply_passive_difference(gateway, request.target, original.passiveIds,
                             request.desiredPassiveIds);
    auto actual = gateway.read_state(request.target);
    if (same_passives(actual.passiveIds, request.desiredPassiveIds)) {
        return result(SkillEditStatus::succeeded, std::move(actual),
                      "Passive preset applied");
    }
    // Task 3 replaces this temporary failure return with rollback.
    return result(SkillEditStatus::failed, std::move(actual),
                  "Game rejected passive preset");
}
```

- [ ] **Step 4: Run tests and verify GREEN**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorTests
ctest --test-dir build -R PalworldEditor.SkillEditor --output-on-failure
```

Expected: all skill editor tests pass, including exact call-count assertions.

- [ ] **Step 5: Commit batch replacement**

```powershell
git add mods/PalworldEditor/inc/skills/skill_editor_service.hpp mods/PalworldEditor/tests/skill_editor_tests.cpp
git commit -m "feat: replace passive skill sets by difference"
```

### Task 3: Complete rollback for partial preset failure

**Files:**
- Modify: `mods/PalworldEditor/inc/skills/skill_editor_service.hpp`
- Modify: `mods/PalworldEditor/tests/skill_editor_tests.cpp`

**Interfaces:**
- Consumes: `detail::apply_passive_difference`.
- Produces: batch results `rolledBack` or `rollbackFailed` based only on reread state.

- [ ] **Step 1: Write failing rollback and rollback-failure tests**

Use the existing fake outcome queues:

```cpp
void test_passive_set_restores_original_after_partial_failure() {
    FakeSkillGateway gateway;
    gateway.state.passiveIds = {"A", "B", "C", "D"};
    gateway.addOutcomes = {true, false, true, true};

    const auto result = skill_editor::execute_skill_edit(
        gateway, passive_set_request({"A", "B", "X", "Y"}));

    CHECK(result.status == skill_editor::SkillEditStatus::rolledBack);
    CHECK(skill_editor::detail::same_passives(
        result.state.passiveIds, std::vector<std::string>{"A", "B", "C", "D"}));
    CHECK(gateway.calls ==
          std::vector<std::string>({"read", "remove:C", "remove:D", "add:X", "add:Y",
                                    "read", "remove:X", "add:C", "add:D", "read"}));
}

void test_passive_set_reports_rollback_failure_without_retry_loop() {
    FakeSkillGateway gateway;
    gateway.state.passiveIds = {"A", "B", "C", "D"};
    gateway.addOutcomes = {true, false, false, true};

    const auto result = skill_editor::execute_skill_edit(
        gateway, passive_set_request({"A", "B", "X", "Y"}));

    CHECK(result.status == skill_editor::SkillEditStatus::rollbackFailed);
    CHECK(gateway.calls.size() == 10);
}
```

Call both tests from `main()`.

- [ ] **Step 2: Run tests and verify RED**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorTests
ctest --test-dir build -R PalworldEditor.SkillEditor --output-on-failure
```

Expected: the new tests fail because Task 2 returns `failed` without restoring the original set.

- [ ] **Step 3: Implement one bounded rollback attempt**

Replace Task 2's temporary failure return:

```cpp
apply_passive_difference(gateway, request.target, actual.passiveIds, original.passiveIds);
auto rolledBack = gateway.read_state(request.target);
if (same_passives(rolledBack.passiveIds, original.passiveIds)) {
    return result(SkillEditStatus::rolledBack, std::move(rolledBack),
                  "Passive preset failed; original skills restored");
}
return result(SkillEditStatus::rollbackFailed, std::move(rolledBack),
              "Passive preset and rollback both failed");
```

Do not add retries, timers, extra reads, or per-skill logging.

- [ ] **Step 4: Run tests and verify GREEN**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorTests
ctest --test-dir build -R PalworldEditor.SkillEditor --output-on-failure
```

Expected: all skill editor tests pass.

- [ ] **Step 5: Commit rollback**

```powershell
git add mods/PalworldEditor/inc/skills/skill_editor_service.hpp mods/PalworldEditor/tests/skill_editor_tests.cpp
git commit -m "fix: roll back failed passive presets"
```

### Task 4: One-request preset factory and ImGui workflow

**Files:**
- Modify: `mods/PalworldEditor/inc/skills/passive_skill_presets.hpp`
- Modify: `mods/PalworldEditor/tests/skill_editor_tests.cpp`
- Modify: `mods/PalworldEditor/src/dllmain.cpp`

**Interfaces:**
- Produces: `make_passive_preset_request(const PassiveSkillPreset&, uint64_t targetGeneration, uint64_t worldGeneration) -> SkillEditRequest`.
- Consumes: `passive_skill_presets`, `find_skill_label`, `SkillEditQueue`, and existing `mutationsDisabled`.

- [ ] **Step 1: Write a failing one-request factory test**

Add:

```cpp
void test_passive_preset_factory_creates_one_world_bound_batch_request() {
    skill_editor::SkillEditQueue queue;
    const auto preset = skill_editor::passive_skill_presets().front();
    queue.push(skill_editor::make_passive_preset_request(preset, 17, 23));

    CHECK(queue.size() == 1);
    const auto request = queue.try_pop();
    CHECK(request.has_value());
    CHECK(request->targetGeneration == 17);
    CHECK(request->worldGeneration == 23);
    CHECK(request->kind == skill_editor::SkillKind::passive);
    CHECK(request->operation == skill_editor::SkillEditOperation::replaceAllPassives);
    CHECK((request->desiredPassiveIds ==
           std::vector<std::string>(preset.passiveIds.begin(), preset.passiveIds.end())));
}
```

Call the test from `main()`.

- [ ] **Step 2: Run tests and verify RED**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorTests
```

Expected: compilation fails because `make_passive_preset_request` does not exist.

- [ ] **Step 3: Implement the value-only request factory**

Include `skill_editor_service.hpp` from the preset header and add:

```cpp
[[nodiscard]] inline auto make_passive_preset_request(
    const PassiveSkillPreset& preset, const std::uint64_t targetGeneration,
    const std::uint64_t worldGeneration) -> SkillEditRequest {
    return {
        .targetGeneration = targetGeneration,
        .worldGeneration = worldGeneration,
        .kind = SkillKind::passive,
        .operation = SkillEditOperation::replaceAllPassives,
        .desiredPassiveIds =
            std::vector<std::string>(preset.passiveIds.begin(), preset.passiveIds.end()),
    };
}
```

- [ ] **Step 4: Run tests and verify GREEN**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorTests
ctest --test-dir build -R PalworldEditor.SkillEditor --output-on-failure
```

Expected: all skill editor tests pass.

- [ ] **Step 5: Add the dropdown and apply button**

In `dllmain.cpp`:

1. Include `skills/passive_skill_presets.hpp`.
2. Add `std::optional<std::size_t> passivePresetIndex_`.
3. Add `std::uint64_t skillUiWorldGeneration_`.
4. Reset the preset index in `reset_skill_editor_ui`.
5. Reset the complete editor UI when either target generation or world generation changes.
6. Render the preset controls above the current passive list.

Replace the existing target-only GUI generation gate with:

```cpp
if (self->skillUiGeneration_ != snapshot.targetGeneration ||
    self->skillUiWorldGeneration_ != snapshot.worldGeneration) {
    self->skillUiGeneration_ = snapshot.targetGeneration;
    self->skillUiWorldGeneration_ = snapshot.worldGeneration;
    reset_skill_editor_ui(self);
}
```

Core UI shape:

```cpp
const auto presets = skill_editor::passive_skill_presets();
const char* preview = self->passivePresetIndex_.has_value()
                          ? presets[*self->passivePresetIndex_].displayName.data()
                          : "请选择预设";
if (ImGui::BeginCombo("词条预设##passive-preset", preview)) {
    for (std::size_t index{}; index < presets.size(); ++index) {
        const bool selected =
            self->passivePresetIndex_.has_value() && *self->passivePresetIndex_ == index;
        if (ImGui::Selectable(presets[index].displayName.data(), selected)) {
            self->passivePresetIndex_ = index;
        }
    }
    ImGui::EndCombo();
}

if (self->passivePresetIndex_.has_value()) {
    const auto& preset = presets[*self->passivePresetIndex_];
    std::string contents;
    for (const auto id : preset.passiveIds) {
        if (!contents.empty()) {
            contents += "、";
        }
        contents += find_skill_label(snapshot.catalog.passive.skills, id);
    }
    ImGui::TextWrapped("预设内容：%s", contents.c_str());
}

ImGui::BeginDisabled(mutationsDisabled || !self->passivePresetIndex_.has_value());
if (ImGui::Button("应用预设") && self->passivePresetIndex_.has_value()) {
    const auto& preset = presets[*self->passivePresetIndex_];
    self->skillQueue_.push(skill_editor::make_passive_preset_request(
        preset, snapshot.targetGeneration, snapshot.worldGeneration));
    self->passiveEditIndex_ = -1;
    self->passiveChoice_.reset();
}
ImGui::EndDisabled();
```

Use scoped ImGui IDs if necessary to avoid collisions. Do not call catalog refresh or Unreal APIs from this renderer.

- [ ] **Step 6: Build the DLL and run all current tests**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor PalworldEditorTests
ctest --test-dir build --output-on-failure
```

Expected: formatting check, DLL build, and both CTest tests pass.

- [ ] **Step 7: Commit the GUI workflow**

```powershell
git add mods/PalworldEditor/inc/skills/passive_skill_presets.hpp mods/PalworldEditor/tests/skill_editor_tests.cpp mods/PalworldEditor/src/dllmain.cpp
git commit -m "feat: apply passive presets from Pal editor"
```

### Task 5: Release documentation and final verification

**Files:**
- Modify: `mods/PalworldEditor/src/dllmain.cpp`
- Modify: `README.md`
- Modify: `AGENTS.md`
- Modify: `CLAUDE.md`

**Interfaces:**
- Publishes: PalworldEditor version `1.6.4`.
- Documents: built-in presets, difference/rollback behavior, localization fallback, and zero steady-state work.

- [ ] **Step 1: Update version and user documentation**

Change all runtime/UI version strings in `dllmain.cpp` from `1.6.3` to `1.6.4`.

Update `README.md` with:

- the preset dropdown and separate apply button;
- exact contents of `工作毕业1` and `工作毕业2`;
- whole-set final semantics with difference-based writes;
- Raw ID language independence and current-language display labels;
- bounded click-only work and zero new idle scanning;
- rollback result behavior.

Update `AGENTS.md` and `CLAUDE.md` with:

- the new preset header in architecture;
- `replaceAllPassives` as one FIFO request;
- no new Hook/background/idle work;
- exact game validation cases.

- [ ] **Step 2: Run the complete repository verification**

Run from an MSVC developer environment:

```powershell
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor PalworldEditorTests PalworldEditorBaseResourceSharingTests
ctest --test-dir build --output-on-failure
git diff --check
```

Expected:

- `PalworldEditor.dll` links successfully;
- `PalworldEditor.SkillEditor` passes;
- `PalworldEditor.BaseResourceSharing` passes;
- formatting and diff checks report no errors.

- [ ] **Step 3: Perform static performance and lifecycle checks**

Run:

```powershell
rg -n "FindAllOf|RegisterHook|wantRefreshSkillCatalog" mods/PalworldEditor/inc/skills/passive_skill_presets.hpp mods/PalworldEditor/inc/skills/skill_editor_service.hpp
rg -n "passivePresetIndex_|replaceAllPassives|make_passive_preset_request" mods/PalworldEditor
git status --short
```

Expected:

- the first search returns no matches;
- the second search finds only the catalog, request service, tests, and GUI integration;
- the paused untracked material-operation plan remains untracked and unstaged.

- [ ] **Step 4: Commit the release update**

```powershell
git add README.md AGENTS.md CLAUDE.md mods/PalworldEditor/src/dllmain.cpp
git commit -m "docs: release PalworldEditor 1.6.4"
```

- [ ] **Step 5: Record game-only acceptance checks**

Do not claim these from unit tests. After manually placing the new DLL in Palworld 1.0.1, verify:

1. Selecting a preset does not modify the Pal.
2. Clicking `应用预设` applies exactly the selected four traits.
3. Reapplying the same preset performs no observable state change.
4. Chinese and another game language display localized names while writing the same Raw IDs.
5. A deliberately rejected add restores the original set or reports `rollbackFailed` without retrying.
6. Switching number-key highlight before applying is rejected by the existing target gate.
7. LoadMap clears queued work and requires target reconfirmation.
8. An idle before/after frame-time capture shows no new steady-state cost; applying once causes no sustained frame-time increase.
