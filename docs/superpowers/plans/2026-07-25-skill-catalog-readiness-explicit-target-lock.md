# Skill Catalog Readiness and Explicit Target Lock Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prevent startup-time skill writes until Palworld runtime services are ready, automatically retry catalog loading, and keep the selected Pal latched until the user explicitly selects again.

**Architecture:** Add a pure-value catalog readiness flag and throttled refresh scheduler, then make the GUI's mutation availability depend on both catalog readiness and an exact match between the latched GUID and the current observation. Continuous target resolution becomes a safety signal only; `confirm()` remains the sole operation that changes the selected target.

**Tech Stack:** C++23, UE4SS experimental runtime, ImGui, CMake/Ninja/MSVC, standard-library-only unit tests.

## Global Constraints

- Target PalworldEditor version is exactly `1.4.5`.
- “刷新技能列表” reloads the complete passive catalog, active catalog, and current-language labels.
- Automatic retries occur at most once every two seconds and stop after runtime readiness is established.
- No skill mutation is enabled before runtime readiness is established.
- Only a successful explicit “选择当前帕鲁” action changes the latched target.
- A missing or different observation never clears or replaces the latched target.
- A missing or different observation disables and rejects writes without caching or guessing a `UObject*`.
- All Unreal reflection calls remain on the `on_update()` game thread.
- GUI/game-thread communication remains pure-value snapshots, atomics, mutex-protected data, and the existing FIFO queue.
- Use RED→GREEN TDD for all new pure C++ behavior.

---

## File Structure

- Modify `mods/PalworldEditor/inc/skills/skill_catalog.hpp`: add runtime readiness, the two-second refresh scheduler, readiness helpers, and merged-state rules.
- Modify `mods/PalworldEditor/inc/skills/selected_target_state.hpp`: expose non-mutating current-observation matching and remove automatic invalidation semantics.
- Modify `mods/PalworldEditor/tests/skill_editor_tests.cpp`: add scheduler, readiness, and explicit-target-lock regression tests.
- Modify `mods/PalworldEditor/src/pal_skills.cpp`: calculate runtime readiness only after complete catalog and localization initialization.
- Modify `mods/PalworldEditor/src/dllmain.cpp`: schedule retries, publish safe-edit state, preserve the selected target, reject unsafe requests without clearing it, and gate every mutation button.
- Modify `README.md`, `AGENTS.md`, and `CLAUDE.md`: update v1.4.5 behavior and verification instructions.

---

### Task 1: Pure Catalog Readiness and Retry Policy

**Files:**
- Modify: `mods/PalworldEditor/inc/skills/skill_catalog.hpp`
- Test: `mods/PalworldEditor/tests/skill_editor_tests.cpp`

**Interfaces:**
- Produces: `SkillCatalogSnapshot::runtimeReady`.
- Produces: `catalog_is_ready_for_editing(const SkillCatalogSnapshot&) -> bool`.
- Produces: `SkillCatalogRefreshScheduler::should_refresh(bool manual, bool ready, time_point now) -> bool`.
- Updates: `with_catalog_fallback()` preserves a previously established `runtimeReady`.

- [ ] **Step 1: Write failing catalog readiness tests**

Add `<chrono>` to the test includes and add:

```cpp
void test_partial_catalog_is_not_ready_for_editing() {
    skill_editor::SkillCatalogSnapshot catalog{
        .passive = {
            .skills = {skill_editor::SkillOption{.id = "PAL_rude"}},
            .ready = false,
        },
        .active = {
            .skills = {skill_editor::SkillOption{
                .id = "Unique_Boar_Tackle", .activeValue = std::uint16_t{15}}},
            .ready = true,
        },
        .runtimeReady = false,
    };

    CHECK(!skill_editor::catalog_is_ready_for_editing(catalog));
    catalog.passive.ready = true;
    CHECK(!skill_editor::catalog_is_ready_for_editing(catalog));
    catalog.runtimeReady = true;
    CHECK(skill_editor::catalog_is_ready_for_editing(catalog));
}

void test_catalog_fallback_preserves_established_runtime_readiness() {
    const skill_editor::SkillCatalogSnapshot previous{
        .passive = {
            .skills = {skill_editor::SkillOption{.id = "PAL_rude"}},
            .ready = true,
        },
        .active = {
            .skills = {skill_editor::SkillOption{
                .id = "Unique_Boar_Tackle", .activeValue = std::uint16_t{15}}},
            .ready = true,
        },
        .runtimeReady = true,
    };
    const skill_editor::SkillCatalogSnapshot failed{
        .passive = {.error = "passive unavailable"},
        .active = {.error = "active unavailable"},
        .runtimeReady = false,
    };

    const auto merged = skill_editor::with_catalog_fallback(previous, failed);
    CHECK(merged.runtimeReady);
    CHECK(skill_editor::catalog_is_ready_for_editing(merged));
}
```

Invoke both tests from `main()`.

- [ ] **Step 2: Build the test target and verify RED**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorTests
```

Expected: compilation fails because `runtimeReady` and
`catalog_is_ready_for_editing()` do not exist.

- [ ] **Step 3: Implement minimal readiness state**

In `skill_catalog.hpp`, add `runtimeReady` to `SkillCatalogSnapshot`, add:

```cpp
[[nodiscard]] inline auto catalog_is_ready_for_editing(
    const SkillCatalogSnapshot& catalog) noexcept -> bool {
    return catalog.runtimeReady && catalog.passive.ready && catalog.active.ready;
}
```

Return `previous.runtimeReady || refreshed.runtimeReady` from
`with_catalog_fallback()`.

- [ ] **Step 4: Build and verify GREEN**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorTests
ctest --test-dir build -R PalworldEditor.SkillEditor --output-on-failure
```

Expected: build succeeds and the selected CTest passes.

- [ ] **Step 5: Write failing scheduler tests**

Add:

```cpp
void test_catalog_refresh_scheduler_throttles_automatic_retries() {
    using namespace std::chrono_literals;
    skill_editor::SkillCatalogRefreshScheduler scheduler{2s};
    const auto start = skill_editor::SkillCatalogRefreshScheduler::time_point{};

    CHECK(scheduler.should_refresh(false, false, start));
    CHECK(!scheduler.should_refresh(false, false, start + 1s));
    CHECK(scheduler.should_refresh(false, false, start + 2s));
    CHECK(!scheduler.should_refresh(false, true, start + 4s));
}

void test_catalog_refresh_scheduler_honors_manual_refresh_immediately() {
    using namespace std::chrono_literals;
    skill_editor::SkillCatalogRefreshScheduler scheduler{2s};
    const auto start = skill_editor::SkillCatalogRefreshScheduler::time_point{};

    CHECK(scheduler.should_refresh(false, false, start));
    CHECK(scheduler.should_refresh(true, false, start + 100ms));
    CHECK(scheduler.should_refresh(true, true, start + 200ms));
}
```

Invoke both tests from `main()`.

- [ ] **Step 6: Build the test target and verify scheduler RED**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorTests
```

Expected: compilation fails because `SkillCatalogRefreshScheduler` does not exist.

- [ ] **Step 7: Implement the scheduler**

Add a `SkillCatalogRefreshScheduler` using `std::chrono::steady_clock`:

```cpp
class SkillCatalogRefreshScheduler {
public:
    using clock = std::chrono::steady_clock;
    using time_point = clock::time_point;

    explicit SkillCatalogRefreshScheduler(clock::duration retryInterval)
        : retryInterval_(retryInterval) {}

    [[nodiscard]] auto should_refresh(bool manual, bool ready, time_point now) -> bool {
        if (manual) {
            nextAutomaticRefresh_ = now + retryInterval_;
            return true;
        }
        if (ready || (hasAttempted_ && now < nextAutomaticRefresh_)) {
            return false;
        }
        hasAttempted_ = true;
        nextAutomaticRefresh_ = now + retryInterval_;
        return true;
    }

private:
    clock::duration retryInterval_;
    time_point nextAutomaticRefresh_{};
    bool hasAttempted_{};
};
```

- [ ] **Step 8: Build and verify scheduler GREEN**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorTests
ctest --test-dir build -R PalworldEditor.SkillEditor --output-on-failure
```

Expected: build succeeds and the selected CTest passes.

---

### Task 2: Explicit Target Lock

**Files:**
- Modify: `mods/PalworldEditor/inc/skills/selected_target_state.hpp`
- Test: `mods/PalworldEditor/tests/skill_editor_tests.cpp`

**Interfaces:**
- Produces: `SelectedTargetState::matches_current(const SelectedTargetObservation&) const -> bool`.
- Removes call-site reliance on `invalidate_if_changed()`.
- Keeps: `confirm()`, `invalidate()`, `generation()`, `current()`, and request-generation `matches()`.

- [ ] **Step 1: Replace the automatic invalidation regression test**

Replace `test_qe_change_invalidates_even_for_same_character_id()` with:

```cpp
void test_observations_do_not_replace_or_clear_explicit_target() {
    skill_editor::SelectedTargetState state;
    CHECK(state.confirm({.identity = identity(10), .name = "Boar"}));
    const auto selectedGeneration = state.generation();

    CHECK(state.matches_current({.identity = identity(10), .name = "Boar"}));
    CHECK(!state.matches_current({}));
    CHECK(!state.matches_current({.identity = identity(20), .name = "SheepBall"}));
    CHECK(state.is_selected());
    CHECK(state.current().identity == identity(10));
    CHECK(state.current().name == "Boar");
    CHECK(state.generation() == selectedGeneration);
}
```

Update the `main()` invocation to the new test name.

- [ ] **Step 2: Build the test target and verify RED**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorTests
```

Expected: compilation fails because `matches_current()` does not exist.

- [ ] **Step 3: Implement non-mutating observation matching**

Add:

```cpp
[[nodiscard]] auto matches_current(
    const SelectedTargetObservation& observation) const noexcept -> bool {
    return selected_ && observation.is_valid() && current_.identity == observation.identity;
}
```

Remove `invalidate_if_changed()` after all production call sites are migrated in Task 3.

- [ ] **Step 4: Build and verify GREEN**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorTests
ctest --test-dir build -R PalworldEditor.SkillEditor --output-on-failure
```

Expected: build succeeds and the selected CTest passes.

---

### Task 3: Runtime Integration and GUI Safety Gate

**Files:**
- Modify: `mods/PalworldEditor/src/pal_skills.cpp`
- Modify: `mods/PalworldEditor/src/dllmain.cpp`

**Interfaces:**
- Consumes: `catalog_is_ready_for_editing()`,
  `SkillCatalogRefreshScheduler::should_refresh()`, and
  `SelectedTargetState::matches_current()`.
- Publishes: `SkillEditorSnapshot::targetMatchesCurrent`.
- Guarantees: no GUI mutation request is created unless the runtime catalog and target match are ready.

- [ ] **Step 1: Compute runtime readiness in `load_catalog()`**

After building and localizing both sections, set:

```cpp
catalog.runtimeReady =
    manager != nullptr && passiveListFunction != nullptr && localizationContextReady &&
    passiveNameFunction != nullptr && activeNameFunction != nullptr && catalog.passive.ready &&
    catalog.active.ready && passiveHasLocalizedNames && activeHasLocalizedNames;
```

Keep Raw ID options and diagnostic errors for partial startup results.

- [ ] **Step 2: Drive catalog refresh through the scheduler**

In `PalworldEditorMod`, add:

```cpp
static constexpr auto kSkillCatalogRetryInterval = std::chrono::seconds{2};
skill_editor::SkillCatalogRefreshScheduler skillCatalogRefreshScheduler_{
    kSkillCatalogRetryInterval};
```

Replace the initial `wantRefreshSkillCatalog_{true}` behavior with:

```cpp
const bool manualRefresh = wantRefreshSkillCatalog_.exchange(false);
const bool catalogReady = [&] {
    const std::lock_guard lock(skillSnapshotMutex_);
    return skill_editor::catalog_is_ready_for_editing(skillSnapshot_.catalog);
}();
const bool refreshRequested = skillCatalogRefreshScheduler_.should_refresh(
    manualRefresh, catalogReady, skill_editor::SkillCatalogRefreshScheduler::clock::now());
```

Keep `load_catalog()` and snapshot publication on the game thread.

- [ ] **Step 3: Preserve the explicit target**

Remove the per-frame `invalidate_if_changed()` call and lifecycle invalidation message.
Compute:

```cpp
const bool targetMatchesCurrent =
    targetResolved && selectedTarget_.matches_current(targetObservation);
```

On a failed “选择当前帕鲁” request, keep `selectedTarget_` and the existing skill state.
On a failed queued edit validation, reject the request and clear only pending queue entries;
do not call `selectedTarget_.invalidate()`.

- [ ] **Step 4: Publish and render safe-edit state**

Add `bool targetMatchesCurrent{}` to `SkillEditorSnapshot` and publish it each frame.
In `render_pal_editor()`, compute:

```cpp
const bool editingReady = snapshot.targetMatchesCurrent &&
                          skill_editor::catalog_is_ready_for_editing(snapshot.catalog);
```

Display a waiting message when runtime readiness is false and a reselect message when a
latched target differs from the current observation.

- [ ] **Step 5: Gate every mutation entry point**

Pass `pending || !editingReady` into both skill renderers as their disabled state.
This must cover:

- passive replace, remove, and add;
- passive picker confirmation;
- active replace, clear, and equip;
- active picker confirmation.

Read-only skill rows remain visible.

- [ ] **Step 6: Remove obsolete automatic invalidation API**

Delete `SelectedTargetState::invalidate_if_changed()` and update its class documentation to
state that only explicit `confirm()` changes the target.

- [ ] **Step 7: Build integration targets**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor PalworldEditorTests
ctest --test-dir build --output-on-failure
```

Expected: all requested targets and the CTest suite pass.

---

### Task 4: Version, Documentation, and Final Verification

**Files:**
- Modify: `mods/PalworldEditor/src/dllmain.cpp`
- Modify: `README.md`
- Modify: `AGENTS.md`
- Modify: `CLAUDE.md`

**Interfaces:**
- Publishes: PalworldEditor version `1.4.5`.
- Documents: automatic catalog retry, full manual refresh, explicit target lock, and safety gate.

- [ ] **Step 1: Update version metadata and GUI title**

Change the mod version and GUI title from `1.4.4` to `1.4.5`, including the startup log.

- [ ] **Step 2: Update user-facing architecture documentation**

Document:

- startup retries the complete catalog every two seconds until ready;
- manual refresh immediately reloads both catalogs and current-language names;
- mutation controls remain disabled before readiness;
- number-key changes do not replace the selected editing target;
- clicking “选择当前帕鲁” is the only way to switch the editing target.

- [ ] **Step 3: Run the complete verification suite**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor PalworldEditorTests
ctest --test-dir build --output-on-failure
git diff --check
git status --short
```

Expected: all build targets succeed, CTest reports `100% tests passed`, `git diff --check`
prints no errors, and status lists only intentional v1.4.5 source, test, and documentation changes.

- [ ] **Step 4: Record the DLL artifact**

Run:

```powershell
Get-FileHash build/Game__Shipping__Win64/bin/PalworldEditor.dll -Algorithm SHA256
```

Expected: a SHA-256 hash for the newly built v1.4.5 DLL, ready for game-side validation.
