# Idle Pal Resolution Performance Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stop PalworldEditor from globally resolving the highlighted party Pal on every EngineTick while preserving immediate write-time validation and a 250-millisecond selected-target consistency check.

**Architecture:** Add a pure C++ state-driven scheduler and pure-value resolution snapshot under `skills/`. `dllmain.cpp` consumes selection/edit requests before deciding whether Unreal resolution is necessary, never retains the returned `UObject*`, and publishes a game-thread-owned skill snapshot to the GUI only after observable changes.

**Tech Stack:** C++23, UE4SS Unreal reflection, `std::chrono::steady_clock`, CMake/Ninja/MSVC, existing single-binary pure C++ tests.

## Global Constraints

- No current-Pal Unreal reflection when no target is confirmed and no selection/edit request exists.
- Selection and skill edit requests must always resolve the Pal immediately in the same EngineTick.
- A confirmed target is revalidated no more than once per 250 milliseconds while idle.
- No `UObject*`, `FProperty*`, Unreal array address, or slot address may survive the current game-thread callback.
- LoadMap must reset the scheduler and continue requiring explicit target reconfirmation.
- This plan must not modify base-resource discovery, preview, or consumption behavior.
- Runtime, GUI, and startup version becomes exactly `1.5.2`.

---

### Task 1: Pure Pal Resolution Scheduler and Value Snapshot

**Files:**
- Create: `mods/PalworldEditor/inc/skills/pal_resolution_scheduler.hpp`
- Modify: `mods/PalworldEditor/inc/skills/selected_target_state.hpp`
- Test: `mods/PalworldEditor/tests/skill_editor_tests.cpp`

**Interfaces:**
- Consumes: `skill_editor::SelectedTargetObservation` and `SelectedTargetResolutionStatus`.
- Produces:
  - `inline constexpr auto kTargetValidationInterval = std::chrono::milliseconds{250};`
  - `enum class PalResolutionTrigger : std::uint8_t { none, selectionRequest, editRequest, validation };`
  - `class PalResolutionScheduler`
  - `PalResolutionScheduler::decide(bool validationRequired, bool selectionRequested, bool editRequested, time_point now) -> PalResolutionTrigger`
  - `PalResolutionScheduler::reset() -> void`
  - `struct TargetResolutionSnapshot` containing only pure values and default equality.

- [ ] **Step 1: Add failing scheduler and value-snapshot tests**

Add the include:

```cpp
#include <skills/pal_resolution_scheduler.hpp>
```

Add tests whose literals independently express the timing contract:

```cpp
void test_pal_resolution_scheduler_has_zero_idle_work() {
    using namespace std::chrono_literals;
    skill_editor::PalResolutionScheduler scheduler;
    const auto start = skill_editor::PalResolutionScheduler::time_point{};

    CHECK(scheduler.decide(false, false, false, start) ==
          skill_editor::PalResolutionTrigger::none);
    CHECK(scheduler.decide(false, false, false, start + 10s) ==
          skill_editor::PalResolutionTrigger::none);
}

void test_pal_resolution_scheduler_throttles_selected_target_validation() {
    using namespace std::chrono_literals;
    skill_editor::PalResolutionScheduler scheduler;
    const auto start = skill_editor::PalResolutionScheduler::time_point{};

    CHECK(scheduler.decide(false, true, false, start) ==
          skill_editor::PalResolutionTrigger::selectionRequest);
    CHECK(scheduler.decide(true, false, false, start + 249ms) ==
          skill_editor::PalResolutionTrigger::none);
    CHECK(scheduler.decide(true, false, false, start + 250ms) ==
          skill_editor::PalResolutionTrigger::validation);
    CHECK(scheduler.decide(true, false, false, start + 499ms) ==
          skill_editor::PalResolutionTrigger::none);
}

void test_pal_resolution_scheduler_never_delays_edit_validation() {
    using namespace std::chrono_literals;
    skill_editor::PalResolutionScheduler scheduler;
    const auto start = skill_editor::PalResolutionScheduler::time_point{};

    CHECK(scheduler.decide(false, true, false, start) ==
          skill_editor::PalResolutionTrigger::selectionRequest);
    CHECK(scheduler.decide(true, false, true, start + 1ms) ==
          skill_editor::PalResolutionTrigger::editRequest);
    CHECK(scheduler.decide(true, false, false, start + 250ms) ==
          skill_editor::PalResolutionTrigger::none);
    CHECK(scheduler.decide(true, false, false, start + 251ms) ==
          skill_editor::PalResolutionTrigger::validation);
}

void test_pal_resolution_scheduler_reset_discards_old_deadline() {
    using namespace std::chrono_literals;
    skill_editor::PalResolutionScheduler scheduler;
    const auto start = skill_editor::PalResolutionScheduler::time_point{};

    CHECK(scheduler.decide(false, true, false, start) ==
          skill_editor::PalResolutionTrigger::selectionRequest);
    scheduler.reset();
    CHECK(scheduler.decide(false, false, false, start + 1s) ==
          skill_editor::PalResolutionTrigger::none);
    CHECK(scheduler.decide(true, false, false, start + 1s) ==
          skill_editor::PalResolutionTrigger::validation);
}

void test_target_resolution_snapshot_equality_tracks_observable_changes() {
    const skill_editor::TargetResolutionSnapshot first{
        .resolved = true,
        .observation = {.identity = identity(10), .name = "Boar"},
        .status = skill_editor::SelectedTargetResolutionStatus::success,
        .holderCandidateCount = 1,
        .localHolderCandidateCount = 1,
        .holderCandidateClasses = L"PalOtomoHolderComponent",
    };
    auto same = first;
    CHECK(first == same);
    same.observation.identity = identity(11);
    CHECK(!(first == same));
}
```

Register all five tests in `main()`.

- [ ] **Step 2: Run the test target and verify RED**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorTests
```

Expected: compilation fails because `skills/pal_resolution_scheduler.hpp`,
`PalResolutionScheduler`, `PalResolutionTrigger`, and `TargetResolutionSnapshot` do not exist.

- [ ] **Step 3: Implement the minimal pure scheduler**

Add default equality to the existing observation:

```cpp
struct SelectedTargetObservation {
    TargetIdentity identity;
    std::string name;

    [[nodiscard]] auto is_valid() const -> bool {
        return identity.is_valid();
    }

    auto operator==(const SelectedTargetObservation&) const -> bool = default;
};
```

Create `pal_resolution_scheduler.hpp`:

```cpp
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include <skills/selected_target_state.hpp>

namespace skill_editor {
inline constexpr auto kTargetValidationInterval = std::chrono::milliseconds{250};

enum class PalResolutionTrigger : std::uint8_t {
    none,
    selectionRequest,
    editRequest,
    validation,
};

class PalResolutionScheduler {
public:
    using clock = std::chrono::steady_clock;
    using time_point = clock::time_point;

    [[nodiscard]] auto decide(bool validationRequired, bool selectionRequested,
                              bool editRequested, time_point now) -> PalResolutionTrigger {
        if (selectionRequested) {
            schedule_next(now);
            return PalResolutionTrigger::selectionRequest;
        }
        if (editRequested) {
            schedule_next(now);
            return PalResolutionTrigger::editRequest;
        }
        if (!validationRequired) {
            nextValidation_.reset();
            return PalResolutionTrigger::none;
        }
        if (!nextValidation_.has_value() || now >= *nextValidation_) {
            schedule_next(now);
            return PalResolutionTrigger::validation;
        }
        return PalResolutionTrigger::none;
    }

    auto reset() noexcept -> void {
        nextValidation_.reset();
    }

private:
    auto schedule_next(time_point now) -> void {
        nextValidation_ = now + kTargetValidationInterval;
    }

    std::optional<time_point> nextValidation_;
};

struct TargetResolutionSnapshot {
    bool resolved{};
    SelectedTargetObservation observation;
    SelectedTargetResolutionStatus status{
        SelectedTargetResolutionStatus::holderCandidatesUnavailable};
    std::size_t holderCandidateCount{};
    std::size_t localHolderCandidateCount{};
    std::wstring holderCandidateClasses;

    auto operator==(const TargetResolutionSnapshot&) const -> bool = default;
};
}  // namespace skill_editor
```

Do not add Unreal types to this header.

- [ ] **Step 4: Run tests and verify GREEN**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorTests
ctest --test-dir build -R PalworldEditor.SkillEditor --output-on-failure
```

Expected: `PalworldEditor.SkillEditor` passes.

- [ ] **Step 5: Commit the pure scheduling contract**

```powershell
git add mods/PalworldEditor/inc/skills/pal_resolution_scheduler.hpp `
        mods/PalworldEditor/inc/skills/selected_target_state.hpp `
        mods/PalworldEditor/tests/skill_editor_tests.cpp
git commit -m "perf: add state-driven Pal resolution scheduler"
```

---

### Task 2: Integrate On-Demand Resolution and Dirty Skill Snapshots

**Files:**
- Modify: `mods/PalworldEditor/src/dllmain.cpp`
- Test: `mods/PalworldEditor/tests/skill_editor_tests.cpp`

**Interfaces:**
- Consumes: `PalResolutionScheduler`, `PalResolutionTrigger`, and
  `TargetResolutionSnapshot` from Task 1.
- Produces:
  - no `resolve_selected_otomo()` call on idle unconfirmed ticks;
  - immediate current-frame resolution for selection/edit requests;
  - 4Hz pure-value consistency validation for a confirmed target;
  - `skillRuntimeSnapshot_` owned by the game thread;
  - `publish_skill_snapshot_if_dirty()` as the only periodic writer to
    GUI-visible `skillSnapshot_`.

- [ ] **Step 1: Add a failing change-tracking test**

Add a small pure helper to the wished-for scheduler interface:

```cpp
void test_target_resolution_state_marks_only_real_changes() {
    skill_editor::TargetResolutionState state;
    const skill_editor::TargetResolutionSnapshot first{
        .resolved = true,
        .observation = {.identity = identity(10), .name = "Boar"},
        .status = skill_editor::SelectedTargetResolutionStatus::success,
        .holderCandidateCount = 1,
        .localHolderCandidateCount = 1,
        .holderCandidateClasses = L"PalOtomoHolderComponent",
    };

    CHECK(state.update(first));
    CHECK(!state.update(first));
    auto changed = first;
    changed.status = skill_editor::SelectedTargetResolutionStatus::parameterUnavailable;
    CHECK(state.update(changed));
    CHECK(state.current() == changed);
    state.reset();
    CHECK(state.current() == skill_editor::TargetResolutionSnapshot{});
}
```

Register the test in `main()`.

- [ ] **Step 2: Run tests and verify RED**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorTests
```

Expected: compilation fails because `TargetResolutionState` does not exist.

- [ ] **Step 3: Implement the minimal value change tracker**

Add to `pal_resolution_scheduler.hpp`:

```cpp
class TargetResolutionState {
public:
    [[nodiscard]] auto update(TargetResolutionSnapshot next) -> bool {
        if (current_ == next) {
            return false;
        }
        current_ = std::move(next);
        return true;
    }

    auto reset() -> void {
        current_ = {};
    }

    [[nodiscard]] auto current() const -> const TargetResolutionSnapshot& {
        return current_;
    }

private:
    TargetResolutionSnapshot current_;
};
```

- [ ] **Step 4: Reorder request consumption before resolution**

Include the scheduler header in `dllmain.cpp` and change `game_thread_tick()` so it:

1. copies and validates the selection request;
2. gives a valid selection request priority and clears pending edits;
3. otherwise pops at most one edit request;
4. asks the scheduler whether reflection is necessary;
5. calls `resolve_selected_otomo()` only when the trigger is not `none`.

Use this control shape:

```cpp
std::optional<skill_editor::WorldBoundRequest> selectionRequest;
{
    const std::lock_guard lock(selectionRequestMutex_);
    selectionRequest = std::exchange(selectCurrentPalRequest_, std::nullopt);
}
const bool selectionRequested =
    selectionRequest.has_value() &&
    skill_editor::request_can_run(*selectionRequest, worldSession_) &&
    worldLifecycleCallbacksReady_.load();

std::optional<skill_editor::SkillEditRequest> editRequest;
if (selectionRequested) {
    skillQueue_.clear();
} else {
    editRequest = skillQueue_.try_pop();
}

const auto trigger = palResolutionScheduler_.decide(
    worldSession_.is_target_confirmed() && selectedTarget_.is_selected(),
    selectionRequested, editRequest.has_value(),
    skill_editor::PalResolutionScheduler::clock::now());

std::optional<pal_game::SelectedPalTarget> resolvedPal;
if (trigger != skill_editor::PalResolutionTrigger::none) {
    resolvedPal = pal_game::resolve_selected_otomo();
}
```

All uses of `selectedPal.parameter` must remain inside the block/tick where `resolvedPal` exists.
Do not store `SelectedPalTarget` as a member.

- [ ] **Step 5: Convert runtime resolution to a pure value**

When resolution occurs, construct:

```cpp
const bool resolved =
    resolvedPal->status == skill_editor::SelectedTargetResolutionStatus::success &&
    resolvedPal->observation.is_valid() && pal_game::is_valid(resolvedPal->parameter);
const skill_editor::TargetResolutionSnapshot nextResolution{
    .resolved = resolved,
    .observation =
        resolved ? resolvedPal->observation : skill_editor::SelectedTargetObservation{},
    .status = resolvedPal->status,
    .holderCandidateCount = resolvedPal->holderCandidateCount,
    .localHolderCandidateCount = resolvedPal->localHolderCandidateCount,
    .holderCandidateClasses = resolvedPal->holderCandidateClasses,
};
skillSnapshotDirty_ = targetResolutionState_.update(nextResolution) || skillSnapshotDirty_;
```

Continue status-change logging only when a real resolution ran. Selection uses the current
`resolvedPal->parameter`; edit execution also requires the current `resolvedPal->parameter`.
Background validation uses only `targetResolutionState_.current()`.

- [ ] **Step 6: Introduce a game-thread-owned snapshot**

Add:

```cpp
skill_editor::PalResolutionScheduler palResolutionScheduler_;
skill_editor::TargetResolutionState targetResolutionState_;
SkillEditorSnapshot skillRuntimeSnapshot_;
bool skillSnapshotDirty_{true};
```

Replace per-frame reads/writes of the shared `skillSnapshot_` with updates to
`skillRuntimeSnapshot_`. Maintain catalog fallback and readiness against the runtime snapshot,
not by locking and copying the GUI snapshot on every tick.

Add:

```cpp
auto publish_skill_snapshot_if_dirty() -> void {
    if (!std::exchange(skillSnapshotDirty_, false)) {
        return;
    }
    const std::lock_guard lock(skillSnapshotMutex_);
    skillSnapshot_ = skillRuntimeSnapshot_;
}
```

Call it once at the end of `game_thread_tick()`. Mark dirty only for selection/edit results,
catalog refresh, changed target resolution, changed target-match/pending state, callback errors,
and world lifecycle changes. Remove the existing unconditional per-frame snapshot mutation block.

- [ ] **Step 7: Preserve write-before-validation semantics**

For selection:

```cpp
if (selectionRequested) {
    const auto& resolution = targetResolutionState_.current();
    if (resolvedPal.has_value() && resolution.resolved &&
        selectedTarget_.confirm(resolution.observation) && worldSession_.confirm_target()) {
        skillRuntimeSnapshot_.state = skillGateway_.read_state(
            reinterpret_cast<skill_editor::SkillTarget>(resolvedPal->parameter));
    } else {
        // Preserve the existing actionable selection failure message.
    }
    skillSnapshotDirty_ = true;
}
```

For edit:

```cpp
if (editRequest.has_value()) {
    const auto& resolution = targetResolutionState_.current();
    const auto target = resolvedPal.has_value() && resolution.resolved
                            ? reinterpret_cast<skill_editor::SkillTarget>(
                                  resolvedPal->parameter)
                            : skill_editor::SkillTarget{};
    editResult = skill_editor::apply_if_target_is_current(
        *editRequest, selectedTarget_, resolution.observation, target, worldSession_,
        [this](const auto& request) {
            return skill_editor::execute_skill_edit(skillGateway_, request);
        });
    // Preserve rejection, reread, rollback, and result-message behavior.
    skillSnapshotDirty_ = true;
}
```

The cached pure observation may drive GUI display, but a non-null current-frame
`resolvedPal->parameter` remains mandatory for every write.

- [ ] **Step 8: Reset scheduling and publish lifecycle changes**

In `begin_world_transition()`:

```cpp
palResolutionScheduler_.reset();
targetResolutionState_.reset();
skillRuntimeSnapshot_.resolutionStatus =
    skill_editor::SelectedTargetResolutionStatus::holderCandidatesUnavailable;
skillSnapshotDirty_ = true;
publish_skill_snapshot_if_dirty();
```

Apply the existing lifecycle fields to `skillRuntimeSnapshot_` before publishing. In
`finish_world_transition()`, reset the scheduler again, update the runtime snapshot, and publish.
If callback registration fails during `on_unreal_init()`, update and publish the runtime snapshot
instead of writing the GUI snapshot directly.

- [ ] **Step 9: Format, build, and run all tests**

Run sequentially so the formatter does not race the compiler on Windows:

```powershell
cmake --build --preset ninja-msvc-x64 --target format
cmake --build --preset ninja-msvc-x64 --target PalworldEditor PalworldEditorTests PalworldEditorBaseResourceSharingTests
ctest --test-dir build --output-on-failure
git diff --check
```

Expected: DLL links, both CTest cases pass, and no PalworldEditor compiler warning is introduced.
The existing third-party PatternSleuth unused-import warning is allowed.

- [ ] **Step 10: Commit runtime integration**

```powershell
git add mods/PalworldEditor/inc/skills/pal_resolution_scheduler.hpp `
        mods/PalworldEditor/src/dllmain.cpp `
        mods/PalworldEditor/tests/skill_editor_tests.cpp
git commit -m "perf: eliminate idle Pal resolution scans"
```

---

### Task 3: Release Notes and Complete Verification

**Files:**
- Modify: `mods/PalworldEditor/src/dllmain.cpp`
- Modify: `README.md`
- Modify: `AGENTS.md`

**Interfaces:**
- Consumes: completed scheduler and dirty-publication behavior.
- Produces: PalworldEditor `1.5.2` runtime strings and game-side validation instructions.

- [ ] **Step 1: Update version and documentation**

Change all current runtime/UI/log version strings from `1.5.1` to `1.5.2`. Document:

- no current-Pal scan runs before explicit target confirmation unless a selection/edit request
  requires one;
- confirmed targets are checked at 250-millisecond intervals;
- selection and edit requests always force immediate write-before-validation;
- no Unreal pointer survives a callback;
- startup item/skill catalog loading remains one-time or retry-based and is not part of the
  steady-state polling fix;
- resource sharing behavior is unchanged by this release.

- [ ] **Step 2: Run complete fresh verification**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor PalworldEditorTests PalworldEditorBaseResourceSharingTests
ctest --test-dir build --output-on-failure
git diff --check
```

Expected: format check and DLL build exit 0, both CTest cases pass, and diff check is silent.

- [ ] **Step 3: Inspect the final diff and prove the idle path**

Run:

```powershell
git diff --stat
git diff -- mods/PalworldEditor/src/dllmain.cpp
rg -n "resolve_selected_otomo" mods/PalworldEditor/src/dllmain.cpp
git status --short
```

Verify from control flow that the sole `resolve_selected_otomo()` call is guarded by a non-`none`
`PalResolutionTrigger`, not executed unconditionally before request inspection.

- [ ] **Step 4: Commit the release metadata**

```powershell
git add AGENTS.md README.md mods/PalworldEditor/src/dllmain.cpp
git commit -m "docs: release PalworldEditor 1.5.2"
```

- [ ] **Step 5: Preserve the branch without deploy or push**

Run:

```powershell
git status --short
git branch --show-current
git log --oneline -4
```

Expected: clean `codex/fix-next-summon-pal` branch. Do not run the `deploy` target and do not push.
