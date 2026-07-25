# PalworldEditor Game-Thread and World-Transition Safety Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move every Palworld Unreal access onto the EngineTick game thread, invalidate edits across LoadMap boundaries, require target reconfirmation after travel, and hide clearly internal active skills.

**Architecture:** `PalworldEditorMod` registers EngineTick and LoadMap callbacks and keeps their callback IDs for destruction-time unregistration. A new pure C++ `WorldSessionState` owns the world generation and reconfirmation invariant; `SkillEditRequest` carries that generation so the domain service rejects stale requests before calling the Unreal gateway. The existing GUI remains a value-only producer/consumer.

**Tech Stack:** C++23, RE-UE4SS callback API, CMake/Ninja/MSVC, the repository's dependency-free C++ test executable and CTest.

## Global Constraints

- All `UObjectGlobals`, `UObject`, `UFunction`, and `ProcessEvent` operations execute only from an EngineTick game-thread callback.
- GUI and UE4SS UpdateThread code may exchange only standard-library values protected by atomics, queues, or named RAII locks.
- LoadMap begin invalidates queued work and removes write authorization; LoadMap completion never restores authorization automatically.
- The previously selected Pal GUID and name remain visible after travel, but the user must click “选择当前帕鲁” again before editing.
- Passive skills continue to come only from `GetPalAssignablePassiveIDs`.
- Active filtering is limited to high-confidence Human, GYM, Raid, and Boss IDs; full per-species learnset validation remains out of scope.
- Do not cache UObject or UFunction pointers across callbacks or frames.
- Preserve C++23, LF line endings, Allman braces, four-space indentation, and the existing repository naming style.

---

## File Structure

- Create `mods/PalworldEditor/inc/skills/world_session_state.hpp`: pure value state for world generation, LoadMap access, and per-generation target confirmation.
- Modify `mods/PalworldEditor/inc/skills/skill_editor_service.hpp`: add `worldGeneration` to requests and reject stale/unconfirmed requests before gateway execution.
- Modify `mods/PalworldEditor/inc/skills/skill_catalog.hpp`: define and apply the high-confidence internal active-skill filter.
- Modify `mods/PalworldEditor/src/dllmain.cpp`: register/unregister hooks, move Unreal work from `on_update()` to EngineTick, and apply LoadMap lifecycle transitions.
- Modify `mods/PalworldEditor/tests/skill_editor_tests.cpp`: add pure tests for session transitions, stale requests, and active filtering.
- Modify `README.md` and `AGENTS.md`: document version 1.4.6 and the corrected game-thread lifecycle contract.

### Task 1: Add the pure world-session state machine

**Files:**
- Create: `mods/PalworldEditor/inc/skills/world_session_state.hpp`
- Modify: `mods/PalworldEditor/tests/skill_editor_tests.cpp`

**Interfaces:**
- Produces: `skill_editor::WorldSessionState`
- Produces: `generation() -> std::uint64_t`
- Produces: `can_access_unreal() -> bool`
- Produces: `is_target_confirmed() -> bool`
- Produces: `begin_transition() -> void`
- Produces: `finish_transition() -> void`
- Produces: `confirm_target() -> bool`
- Produces: `request_targets_current_world(std::uint64_t) -> bool`
- Produces: `request_is_current(std::uint64_t) -> bool`

- [ ] **Step 1: Write failing session-state tests**

Add the header include and tests equivalent to:

```cpp
#include <skills/world_session_state.hpp>

void test_world_session_transition_requires_reconfirmation()
{
    skill_editor::WorldSessionState session;
    CHECK(session.can_access_unreal());
    CHECK(session.confirm_target());
    CHECK(session.is_target_confirmed());

    session.begin_transition();
    CHECK(session.generation() == 1);
    CHECK(!session.can_access_unreal());
    CHECK(!session.is_target_confirmed());
    CHECK(!session.request_is_current(0));

    session.finish_transition();
    CHECK(session.can_access_unreal());
    CHECK(!session.is_target_confirmed());
    CHECK(session.request_targets_current_world(1));
    CHECK(!session.request_is_current(1));

    CHECK(session.confirm_target());
    CHECK(session.request_is_current(1));
}

void test_world_session_cannot_confirm_during_transition()
{
    skill_editor::WorldSessionState session;
    session.begin_transition();
    CHECK(!session.confirm_target());
    CHECK(!session.is_target_confirmed());
}
```

Call both functions from the test executable's `main()`.

- [ ] **Step 2: Build the test target and verify RED**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorTests
```

Expected: compilation fails because `skills/world_session_state.hpp` does not exist.

- [ ] **Step 3: Implement the minimal state machine**

Create a self-contained header with this invariant:

```cpp
namespace skill_editor
{
class WorldSessionState
{
public:
    [[nodiscard]] auto generation() const noexcept -> std::uint64_t;
    [[nodiscard]] auto can_access_unreal() const noexcept -> bool;
    [[nodiscard]] auto is_target_confirmed() const noexcept -> bool;
    auto begin_transition() noexcept -> void;
    auto finish_transition() noexcept -> void;
    [[nodiscard]] auto confirm_target() noexcept -> bool;
    [[nodiscard]] auto request_targets_current_world(std::uint64_t requestGeneration) const noexcept -> bool;
    [[nodiscard]] auto request_is_current(std::uint64_t requestGeneration) const noexcept -> bool;

private:
    std::uint64_t generation_{};
    bool transitioning_{};
    std::optional<std::uint64_t> confirmedGeneration_;
};
}
```

`begin_transition()` increments the generation, marks transitioning, and clears confirmation. `finish_transition()` only clears transitioning. Confirmation succeeds only when not transitioning. A selection request targets the current world when Unreal access is allowed and its generation equals the current generation. A skill-edit request is current only when that world is also confirmed.

- [ ] **Step 4: Build and run tests to verify GREEN**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorTests
ctest --test-dir build --output-on-failure
```

Expected: build succeeds and `PalworldEditor.SkillEditor` passes.

- [ ] **Step 5: Commit the state machine**

```powershell
git add mods/PalworldEditor/inc/skills/world_session_state.hpp mods/PalworldEditor/tests/skill_editor_tests.cpp
git commit -m "test: define world transition edit safety"
```

### Task 2: Bind skill requests to a world generation

**Files:**
- Modify: `mods/PalworldEditor/inc/skills/skill_editor_service.hpp`
- Modify: `mods/PalworldEditor/tests/skill_editor_tests.cpp`

**Interfaces:**
- Consumes: `SkillEditRequest::targetGeneration`
- Produces: `SkillEditRequest::worldGeneration`
- Changes: `apply_if_target_is_current(...)` receives `const WorldSessionState& session`

- [ ] **Step 1: Add failing stale-world and unconfirmed-world tests**

Extend the existing `apply_if_target_is_current` tests:

```cpp
skill_editor::WorldSessionState session;
CHECK(session.confirm_target());

const auto accepted = skill_editor::apply_if_target_is_current(
    {.targetGeneration = state.generation(), .worldGeneration = session.generation()},
    state, observed, 0x2000, session, apply);
CHECK(accepted.has_value());

session.begin_transition();
session.finish_transition();
const auto staleWorld = skill_editor::apply_if_target_is_current(
    {.targetGeneration = state.generation(), .worldGeneration = 0},
    state, observed, 0x2000, session, apply);
CHECK(!staleWorld.has_value());

const auto unconfirmedWorld = skill_editor::apply_if_target_is_current(
    {.targetGeneration = state.generation(), .worldGeneration = session.generation()},
    state, observed, 0x2000, session, apply);
CHECK(!unconfirmedWorld.has_value());
```

- [ ] **Step 2: Build and verify RED**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorTests
```

Expected: compilation fails because `SkillEditRequest` lacks `worldGeneration` and the apply helper lacks the session parameter.

- [ ] **Step 3: Implement minimal request validation**

Include `world_session_state.hpp`, add:

```cpp
std::uint64_t worldGeneration{};
```

to `SkillEditRequest`, then require both:

```cpp
request.targetGeneration == selectedTarget.generation()
session.request_is_current(request.worldGeneration)
```

before invoking the supplied apply callback. Preserve the existing GUID comparison and target-handle checks.

- [ ] **Step 4: Build and run tests to verify GREEN**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorTests
ctest --test-dir build --output-on-failure
```

Expected: all tests pass and the apply callback count does not change for stale/unconfirmed requests.

- [ ] **Step 5: Commit request-generation validation**

```powershell
git add mods/PalworldEditor/inc/skills/skill_editor_service.hpp mods/PalworldEditor/tests/skill_editor_tests.cpp
git commit -m "fix: reject skill edits from stale worlds"
```

### Task 3: Filter high-confidence internal active skills

**Files:**
- Modify: `mods/PalworldEditor/inc/skills/skill_catalog.hpp`
- Modify: `mods/PalworldEditor/tests/skill_editor_tests.cpp`

**Interfaces:**
- Produces: `is_internal_active_skill_id(std::string_view) -> bool`
- Changes: `make_active_skill_options(...)` omits definitions for which the predicate returns true

- [ ] **Step 1: Write failing filter tests**

Add:

```cpp
void test_internal_active_skill_filter()
{
    CHECK(skill_editor::is_internal_active_skill_id("Human_Punch"));
    CHECK(skill_editor::is_internal_active_skill_id("Unique_MoonQueen_GYM_Act"));
    CHECK(skill_editor::is_internal_active_skill_id("RaidCutter"));
    CHECK(skill_editor::is_internal_active_skill_id("Unique_LilyQueen_LilyHealing_Boss"));
    CHECK(!skill_editor::is_internal_active_skill_id("SelfDestruct"));
    CHECK(!skill_editor::is_internal_active_skill_id("MudShot"));
    CHECK(!skill_editor::is_internal_active_skill_id("Unique_Boar_Tackle"));

    const std::array definitions{
        skill_editor::ActiveSkillDefinition{1, "Human_Punch"},
        skill_editor::ActiveSkillDefinition{15, "Unique_Boar_Tackle"},
        skill_editor::ActiveSkillDefinition{124, "MudShot"},
    };
    const auto options = skill_editor::make_active_skill_options(
        definitions, [](const auto&) { return std::string{}; });
    CHECK(options.size() == 2);
    CHECK(options[0].id == "Unique_Boar_Tackle");
    CHECK(options[1].id == "MudShot");
}
```

- [ ] **Step 2: Build and verify RED**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorTests
```

Expected: compilation fails because `is_internal_active_skill_id` does not exist, or the options count is three.

- [ ] **Step 3: Implement the minimal predicate and catalog filtering**

Implement a case-sensitive predicate over generated stable Raw IDs:

```cpp
return id.starts_with("Human_") || id.contains("_GYM_") || id.contains("Raid") ||
       id.contains("Boss");
```

Skip matching definitions inside `make_active_skill_options` before localizing or appending them.

- [ ] **Step 4: Build and run tests to verify GREEN**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorTests
ctest --test-dir build --output-on-failure
```

Expected: all tests pass; normal and Unique Pal skills remain in order.

- [ ] **Step 5: Commit active-skill filtering**

```powershell
git add mods/PalworldEditor/inc/skills/skill_catalog.hpp mods/PalworldEditor/tests/skill_editor_tests.cpp
git commit -m "fix: hide internal active skills"
```

### Task 4: Move Unreal work to EngineTick and gate LoadMap

**Files:**
- Modify: `mods/PalworldEditor/src/dllmain.cpp`
- Modify: `mods/PalworldEditor/tests/skill_editor_tests.cpp`

**Interfaces:**
- Consumes: `WorldSessionState`
- Consumes: `SkillEditRequest::worldGeneration`
- Produces: `game_thread_tick() -> void`
- Produces: `begin_world_transition() -> void`
- Produces: `finish_world_transition() -> void`
- Produces: stored `RC::Unreal::Hook::GlobalCallbackId` values for EngineTick and LoadMap callbacks

- [ ] **Step 1: Add a failing world-bound selection-request test helper**

Add a small pure request type to `world_session_state.hpp`:

```cpp
struct WorldBoundRequest
{
    std::uint64_t worldGeneration{};
};

[[nodiscard]] auto request_can_run(const WorldBoundRequest& request,
                                   const WorldSessionState& session) noexcept -> bool;
```

Test that a request created before `begin_transition()` is rejected after `finish_transition()`, while a selection request created for the new generation is accepted without pre-existing target confirmation.

- [ ] **Step 2: Build and verify RED**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorTests
```

Expected: compilation fails because `WorldBoundRequest` and `request_can_run` do not exist.

- [ ] **Step 3: Implement the minimal pure request helper**

Implement `request_can_run` by delegating to
`session.request_targets_current_world(request.worldGeneration)`. Target confirmation is established by the accepted selection request, so it cannot be a prerequisite for that request.

- [ ] **Step 4: Run tests to verify the helper is GREEN**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorTests
ctest --test-dir build --output-on-failure
```

Expected: all tests pass.

- [ ] **Step 5: Replace `on_update()` Unreal work with `game_thread_tick()`**

In `dllmain.cpp`:

- Include `Unreal/Hooks/Hooks.hpp` and `skills/world_session_state.hpp`.
- Leave `on_update()` absent or empty so UE4SS UpdateThread never touches Unreal or runtime queues.
- Move the existing body into private `game_thread_tick()`.
- Update comments that incorrectly identify `on_update()` as the game-thread entry.
- Add `WorldSessionState worldSession_`.
- Add `worldGeneration` to `SkillEditorSnapshot`.
- Add the snapshot world generation to every GUI-created `SkillEditRequest`.
- Pass `worldSession_` to `apply_if_target_is_current`.
- On successful “选择当前帕鲁”, call `worldSession_.confirm_target()` before publishing an editable state.

- [ ] **Step 6: Register and unregister UE4SS callbacks**

In `on_unreal_init()` register:

```cpp
Hook::RegisterEngineTickPreCallback(
    [this](auto&, UEngine*, float, bool) { game_thread_tick(); },
    {.bOnce = false, .bReadonly = true, .OwnerModName = STR("PalworldEditor"),
     .HookName = STR("GameThreadTick")});
```

Register analogous LoadMap pre/post callbacks that call the transition methods. Store all returned IDs. In `~PalworldEditorMod()`, call `Hook::UnregisterCallback` for every non-error ID and log failures.

Do not perform the existing `StaticFindObject` probe in `on_unreal_init()`; move it behind a one-shot request consumed by `game_thread_tick()`.

- [ ] **Step 7: Implement world-transition cleanup**

`begin_world_transition()` must:

- call `worldSession_.begin_transition()`;
- clear `skillQueue_`;
- clear selection request state;
- clear give/modify/read/scan/discover request flags;
- clear inventory/item and current skill-state snapshots under their respective locks;
- retain `selectedTarget_` so the Pal name/GUID remain available;
- publish `targetMatchesCurrent = false`, `pending = false`, and the reconfirmation message.

`finish_world_transition()` must:

- call `worldSession_.finish_transition()`;
- request new inventory/item/skill catalog scans;
- leave target write confirmation false.

Replace the selection atomic with a mutex-protected `std::optional<WorldBoundRequest>`. GUI submission captures the snapshot world generation. The game thread accepts it only when that generation equals the current accessible generation; selection success then confirms the target for that generation.

- [ ] **Step 8: Compile the mod and run all tests**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditor PalworldEditorTests
ctest --test-dir build --output-on-failure
```

Expected: both targets build; all tests pass; no deprecated Hook overload is used.

- [ ] **Step 9: Commit EngineTick integration**

```powershell
git add mods/PalworldEditor/src/dllmain.cpp mods/PalworldEditor/inc/skills/world_session_state.hpp mods/PalworldEditor/tests/skill_editor_tests.cpp
git commit -m "fix: run Pal editing on the game thread"
```

### Task 5: Release metadata and full verification

**Files:**
- Modify: `mods/PalworldEditor/src/dllmain.cpp`
- Modify: `README.md`
- Modify: `AGENTS.md`

**Interfaces:**
- Changes: displayed and logged mod version from `1.4.5` to `1.4.6`
- Documents: EngineTick-only Unreal access and cross-world reconfirmation behavior

- [ ] **Step 1: Update release metadata and documentation**

Update all PalworldEditor version strings to `1.4.6`. Replace documentation that says `on_update()` is the game-thread lifecycle entry with the EngineTick callback contract. Document that LoadMap clears pending operations and requires explicit target reconfirmation.

- [ ] **Step 2: Run formatting and complete verification**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor PalworldEditorTests
ctest --test-dir build --output-on-failure
git diff --check
```

Expected: every command exits zero and CTest reports 100% passing.

- [ ] **Step 3: Inspect the final diff**

Run:

```powershell
git diff --stat HEAD~4
git diff --check
git status --short
```

Expected: only the planned mod, tests, README, AGENTS, design, and plan files are changed or committed; no build products are tracked.

- [ ] **Step 4: Commit release metadata**

```powershell
git add mods/PalworldEditor/src/dllmain.cpp README.md AGENTS.md
git commit -m "docs: release PalworldEditor 1.4.6"
```

- [ ] **Step 5: Record game-only validation**

Report that automated tests cover pure state and catalog rules, while the following still require the user's game session:

- repeated exit/re-enter cycles;
- absence of high-frequency status 3/0 flicker;
- successful active/passive modifications after explicit reconfirmation;
- absence of LoadMap crashes.
