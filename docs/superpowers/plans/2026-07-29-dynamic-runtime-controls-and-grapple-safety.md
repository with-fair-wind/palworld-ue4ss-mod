# Dynamic Runtime Controls and Grapple Safety Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove persisted configuration for the grappling-hook no-cooldown and cross-base resource-sharing switches, make both controls process-local and default-off, and make the grappling override apply safely after the current world and player inventory are ready without requiring an off/on toggle.

**Architecture:** Keep GUI state as atomically transferred pure values and keep all Unreal work on EngineTick. The grappling domain ledger becomes an explicit finite-state machine whose `targetUnavailable` result is retryable rather than terminal; the Unreal gateway remains a one-shot, strict-ID scan with per-object restoration records. LoadMap restores active mutations before changing generation, preserves only the user's process-local desired values, and re-enters readiness gating in the next world.

**Tech Stack:** C++23, CMake 3.22+, Ninja/MSVC, RE-UE4SS experimental, Unreal reflection on the game thread, ImGui, standard-library-only CTest coverage.

## Global Constraints

- Both features are `false` on every DLL load. Do not read, create, migrate, or write `config.ini`.
- Toggling either checkbox changes only the current process. Re-entering a world in the same process preserves the user's desired value; restarting the game resets both to off.
- ImGui callbacks may update atomics and pure-value requests only. They must not find Unreal objects, inspect properties, call `ProcessEvent`, register hooks, or mutate arrays.
- All UObject lookup, reflection, `ProcessEvent`, cooldown mutation, and restoration happen on EngineTick or LoadMap game-thread callbacks.
- Never retain `UObject*`, `UFunction*`, `FProperty*`, array addresses, or parameter buffers across callbacks.
- With a feature off, it must not register its functional UFunction hooks, scan related objects, reconcile resources, or write game state.
- The grapple gateway may call `FindAllOf("PalWeaponBase")` only for an explicit one-shot apply attempt after the current world, local player, and Common main inventory are ready.
- `targetUnavailable` is retryable and must not set the "apply completed" bit. Layout or verification failures safety-disable the feature for the current world.
- No timer-driven or per-frame repeated weapon scan. Retry sources are: the first readiness transition, a user click on “重试应用”, and a later separately verified equipment-change event if one is added after this plan.
- Preserve exact Raw ID filtering, `CoolDownTime = 0.1F`, per-object original values, read-back verification, and full restoration on disable and LoadMap.
- Never throw through a hook or Unreal callback. Use structured status values and fail closed.
- Do not change the resource union algorithm in this plan. The follow-up resource-sharing plan depends on the process-local toggle removal completed here.
- Implement pure-domain behavior with TDD and observe the RED result before changing production code.
- Keep unrelated user files and changes untouched, especially `docs/superpowers/plans/2026-07-26-extensible-material-operation-sessions.md`.
- Target release version remains `1.6.7` while this intermediate plan is implemented; the follow-up resource correctness plan performs the combined version bump.

## File Structure

- **Modify:** `mods/PalworldEditor/inc/grappling_hook/cooldown_service.hpp` — replace the terminal “attempted” bit with explicit apply outcomes, retry authorization, and current-world safety state.
- **Modify:** `mods/PalworldEditor/inc/grappling_hook/cooldown_gateway.hpp` — keep the existing gateway result categories and expose a small status-to-domain mapping helper if useful.
- **Modify:** `mods/PalworldEditor/src/grapple_cooldown_gateway.cpp` — preserve strict one-shot scanning, add no background retry, and keep all validation/rollback local to one call.
- **Modify:** `mods/PalworldEditor/src/dllmain.cpp` — remove configuration I/O, add readiness-gated apply and explicit retry request, retain desired state across LoadMap, and update GUI status.
- **Delete:** `mods/PalworldEditor/inc/editor/settings.hpp` — obsolete persisted aggregate settings.
- **Delete:** `mods/PalworldEditor/inc/base_resource_sharing/settings.hpp` — obsolete persisted resource switch value.
- **Delete:** `mods/PalworldEditor/inc/grappling_hook/settings.hpp` — obsolete persisted grapple switch value.
- **Delete:** `mods/PalworldEditor/src/editor_settings.cpp` — obsolete INI parsing and atomic file persistence.
- **Modify:** `mods/PalworldEditor/tests/base_resource_sharing_tests.cpp` — remove INI tests and add grapple retry/readiness/world-generation regressions.
- **Modify:** `mods/PalworldEditor/CMakeLists.txt` — remove the deleted settings source from DLL and test targets.
- **Modify:** `README.md`, `AGENTS.md`, `CLAUDE.md` — describe process-local default-off controls, readiness retry, and the absence of `config.ini`.

---

## Task 1: Remove Persisted Settings Without Changing Runtime Semantics

**Files:**

- Modify: `mods/PalworldEditor/tests/base_resource_sharing_tests.cpp`
- Modify: `mods/PalworldEditor/CMakeLists.txt`
- Modify: `mods/PalworldEditor/src/dllmain.cpp`
- Delete: `mods/PalworldEditor/inc/editor/settings.hpp`
- Delete: `mods/PalworldEditor/inc/base_resource_sharing/settings.hpp`
- Delete: `mods/PalworldEditor/inc/grappling_hook/settings.hpp`
- Delete: `mods/PalworldEditor/src/editor_settings.cpp`

**Interfaces:**

- Removes `editor_settings::Settings`, `parse_settings`, `load_settings`, `save_settings`, `configPath_`, `baseSharingConfigError_`, and `grappleConfigError_`.
- Retains `baseSharingRequested_`, `baseSharingSettingDirty_`, `grappleRequested_`, and `grappleSettingDirty_` as the ImGui-to-game-thread handoff.
- Establishes `false` member initialization as the only startup source of truth.

- [ ] **Step 1: Delete the persistence tests and prove the old source is still linked**

Remove `#include <editor/settings.hpp>`, `test_settings_default_off_and_round_trip()`,
`test_settings_file_round_trip()`, and their two calls from
`mods/PalworldEditor/tests/base_resource_sharing_tests.cpp`.

Then remove only `src/editor_settings.cpp` from the test target, not yet from the DLL target:

```cmake
add_executable(PalworldEditorBaseResourceSharingTests
    tests/base_resource_sharing_tests.cpp
)
```

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorBaseResourceSharingTests
```

Expected: GREEN for the test executable. This confirms settings tests are isolated and the remaining
production dependency is only in `dllmain.cpp`.

- [ ] **Step 2: Remove startup configuration loading and observe the production compile failure**

In `mods/PalworldEditor/src/dllmain.cpp`:

- remove `#include <editor/settings.hpp>`;
- remove `configPath_` and both configuration-error members;
- remove the `on_program_start()` code that derives the mod path and calls `load_settings`;
- leave the old GUI `save_settings(...)` calls temporarily so the compiler identifies every remaining
  persistence dependency.

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditor
```

Expected: RED with unresolved `editor_settings`, `configPath_`, or configuration-error references in the
two checkbox render blocks. Record this failure before continuing.

- [ ] **Step 3: Convert both GUI controls to process-local requests**

Replace each persistence block with a pure request handoff. The intended pattern is:

```cpp
bool requested = baseSharingRequested_.load(std::memory_order_acquire);
if (ImGui::Checkbox(u8"同公会跨据点资源共享", &requested)) {
    baseSharingRequested_.store(requested, std::memory_order_release);
    baseSharingSettingDirty_.store(true, std::memory_order_release);
}
```

and:

```cpp
bool requested = grappleRequested_.load(std::memory_order_acquire);
if (ImGui::Checkbox(u8"爪钩枪无冷却", &requested)) {
    grappleRequested_.store(requested, std::memory_order_release);
    grappleSettingDirty_.store(true, std::memory_order_release);
}
```

Do not call the bridges directly from ImGui. Remove configuration error rendering. Add one shared neutral
line near the controls:

```cpp
ImGui::TextDisabled(u8"仅本次游戏进程有效；重新启动游戏后默认关闭。");
```

- [ ] **Step 4: Delete the obsolete settings implementation**

Remove the four settings files and the DLL source entry:

```cmake
add_library(${TARGET} SHARED
    src/dllmain.cpp
    src/pal_skills.cpp
    src/pal_stats.cpp
    src/grapple_cooldown_gateway.cpp
    src/pal_base_resource_runtime.cpp
    src/pal_base_resources.cpp
)
```

Run:

```powershell
rg -n "editor_settings|configPath_|ConfigError|save_settings|load_settings|config\\.ini|settings\\.hpp" `
  mods/PalworldEditor CMakeLists.txt
cmake --build --preset ninja-msvc-x64 --target PalworldEditor PalworldEditorBaseResourceSharingTests
```

Expected: `rg` returns no production references; both targets build.

- [ ] **Step 5: Run focused tests**

```powershell
ctest --test-dir build -R PalworldEditorBaseResourceSharingTests --output-on-failure
git diff --check
```

Expected: one passing resource-sharing test target and no whitespace errors.

- [ ] **Step 6: Commit the persistence removal**

```powershell
git add mods/PalworldEditor/CMakeLists.txt `
  mods/PalworldEditor/src/dllmain.cpp `
  mods/PalworldEditor/tests/base_resource_sharing_tests.cpp `
  mods/PalworldEditor/inc/editor/settings.hpp `
  mods/PalworldEditor/inc/base_resource_sharing/settings.hpp `
  mods/PalworldEditor/inc/grappling_hook/settings.hpp `
  mods/PalworldEditor/src/editor_settings.cpp
git commit -m "refactor: make runtime feature controls process local"
```

---

## Task 2: Make Grapple Apply Outcomes Explicit and Retryable

**Files:**

- Modify: `mods/PalworldEditor/inc/grappling_hook/cooldown_service.hpp`
- Modify: `mods/PalworldEditor/tests/base_resource_sharing_tests.cpp`

**Interfaces:**

Add:

```cpp
enum class CooldownApplyOutcome : std::uint8_t {
    succeeded,
    targetUnavailable,
    terminalFailure,
};

enum class CooldownRuntimePhase : std::uint8_t {
    off,
    waitingForWorld,
    readyToApply,
    applying,
    active,
    waitingForRetry,
    restoring,
    safetyDisabled,
};
```

Replace `mark_apply_attempted(...)` with:

```cpp
[[nodiscard]] auto begin_apply(std::uint64_t generation) noexcept -> bool;
[[nodiscard]] auto complete_apply(
    std::uint64_t generation,
    CooldownApplyOutcome outcome,
    std::vector<CooldownOverrideRecord> records = {}) noexcept -> bool;
[[nodiscard]] auto request_retry(std::uint64_t generation) noexcept -> bool;
[[nodiscard]] auto phase(std::uint64_t generation) const noexcept -> CooldownRuntimePhase;
```

`next_work(...)` remains the single dispatcher query. It returns `apply` only after readiness and only
when not applying, waiting for explicit retry, active, or safety-disabled.

- [ ] **Step 1: Write failing target-unavailable and retry tests**

Replace the happy-path use of `mark_apply_attempted` and add:

```cpp
void test_grapple_target_unavailable_waits_for_explicit_retry() {
    using namespace grappling_hook;

    CooldownOverrideLedger ledger;
    CHECK(ledger.begin_world(7));
    ledger.set_desired(true);
    CHECK(ledger.next_work(7, true) == CooldownWork::apply);
    CHECK(ledger.begin_apply(7));
    CHECK(ledger.complete_apply(7, CooldownApplyOutcome::targetUnavailable));

    CHECK(ledger.phase(7) == CooldownRuntimePhase::waitingForRetry);
    CHECK(ledger.next_work(7, true) == CooldownWork::none);
    CHECK(!ledger.request_retry(8));
    CHECK(ledger.request_retry(7));
    CHECK(ledger.next_work(7, true) == CooldownWork::apply);
}
```

Add:

```cpp
void test_grapple_terminal_failure_is_not_reenabled_by_retry() {
    using namespace grappling_hook;

    CooldownOverrideLedger ledger;
    CHECK(ledger.begin_world(7));
    ledger.set_desired(true);
    CHECK(ledger.begin_apply(7));
    CHECK(ledger.complete_apply(7, CooldownApplyOutcome::terminalFailure));
    CHECK(ledger.phase(7) == CooldownRuntimePhase::safetyDisabled);
    CHECK(!ledger.request_retry(7));
    CHECK(ledger.next_work(7, true) == CooldownWork::none);
}
```

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorBaseResourceSharingTests
```

Expected: RED because the new enums and methods do not exist.

- [ ] **Step 2: Implement the finite-state ledger**

Replace the `applyAttempted_` boolean with:

```cpp
bool applyInFlight_{};
bool retryRequired_{};
bool safetyDisabled_{};
```

Implement the transitions:

```cpp
auto begin_apply(const std::uint64_t generation) noexcept -> bool {
    if (generation != generation_ || !desired_ || applyInFlight_ || retryRequired_ ||
        safetyDisabled_ || !records_.empty()) {
        return false;
    }
    applyInFlight_ = true;
    return true;
}

auto complete_apply(const std::uint64_t generation, const CooldownApplyOutcome outcome,
                    std::vector<CooldownOverrideRecord> records) noexcept -> bool {
    if (generation != generation_ || !applyInFlight_) {
        return false;
    }
    applyInFlight_ = false;
    switch (outcome) {
    case CooldownApplyOutcome::succeeded:
        records_ = std::move(records);
        retryRequired_ = false;
        return !records_.empty();
    case CooldownApplyOutcome::targetUnavailable:
        records_.clear();
        retryRequired_ = true;
        return true;
    case CooldownApplyOutcome::terminalFailure:
        records_.clear();
        safetyDisabled_ = true;
        retryRequired_ = false;
        return true;
    }
    return false;
}
```

Also enforce:

- `begin_world(newGeneration)` refuses while restoration records exist;
- a successful new world resets in-flight/retry/safety flags;
- `set_desired(false)` schedules restoration when records exist and otherwise returns to `off`;
- changing false to true clears a retry wait but does not clear `safetyDisabled_` in the same world;
- `request_retry` only succeeds for matching generation, desired-on, `retryRequired_`, no records, and
  not safety-disabled;
- `complete_restore(true)` clears records and returns to off/waiting-world according to desired state.

- [ ] **Step 3: Add generation and restore regressions**

Add:

```cpp
void test_grapple_retry_state_is_reset_only_by_a_new_world() {
    using namespace grappling_hook;

    CooldownOverrideLedger ledger;
    CHECK(ledger.begin_world(7));
    ledger.set_desired(true);
    CHECK(ledger.begin_apply(7));
    CHECK(ledger.complete_apply(7, CooldownApplyOutcome::terminalFailure));
    ledger.set_desired(false);
    ledger.set_desired(true);
    CHECK(ledger.phase(7) == CooldownRuntimePhase::safetyDisabled);
    CHECK(ledger.begin_world(8));
    CHECK(ledger.phase(8) == CooldownRuntimePhase::readyToApply);
}
```

Update existing restore tests to use:

```cpp
CHECK(ledger.begin_apply(7));
CHECK(ledger.complete_apply(
    7, CooldownApplyOutcome::succeeded,
    {{.objectFullName = L"PalWeaponBase /Game/GrappleA", .originalCooldown = 12.0F},
     {.objectFullName = L"PalWeaponBase /Game/GrappleB", .originalCooldown = 6.0F}}));
```

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorBaseResourceSharingTests
ctest --test-dir build -R PalworldEditorBaseResourceSharingTests --output-on-failure
```

Expected: GREEN.

- [ ] **Step 4: Commit the pure-domain state machine**

```powershell
git add mods/PalworldEditor/inc/grappling_hook/cooldown_service.hpp `
  mods/PalworldEditor/tests/base_resource_sharing_tests.cpp
git commit -m "fix: make grapple application safely retryable"
```

---

## Task 3: Gate the One-Shot Grapple Scan on World and Player Readiness

**Files:**

- Modify: `mods/PalworldEditor/src/dllmain.cpp`
- Modify: `mods/PalworldEditor/inc/grappling_hook/cooldown_gateway.hpp`
- Modify: `mods/PalworldEditor/src/grapple_cooldown_gateway.cpp`

**Interfaces:**

Add to `PalworldEditorMod`:

```cpp
std::atomic_bool grappleRetryRequested_{false};

[[nodiscard]] auto grapple_world_ready() -> bool;
auto request_grapple_retry() noexcept -> void;
auto publish_grapple_status(std::string status) -> void;
```

`grapple_world_ready()` is game-thread-only and uses the existing safe gate:

```cpp
return worldSession_.can_read() &&
       pal_game::is_valid(pal_game::get_main_container());
```

It must not retain the returned container.

- [ ] **Step 1: Add the readiness gate before gateway apply**

In the existing EngineTick grapple dispatcher:

1. consume `grappleSettingDirty_` and call `ledger.set_desired(...)`;
2. consume `grappleRetryRequested_` and call `ledger.request_retry(currentGeneration)`;
3. compute readiness exactly once for this dispatch;
4. call `next_work(generation, readiness)`;
5. call `begin_apply(generation)` immediately before `gateway.apply()`;
6. map the gateway result to the domain outcome.

The mapping must be explicit:

```cpp
switch (result.status) {
case grappling_hook::CooldownGatewayStatus::succeeded:
    ledger.complete_apply(generation, grappling_hook::CooldownApplyOutcome::succeeded,
                          std::move(result.records));
    break;
case grappling_hook::CooldownGatewayStatus::targetUnavailable:
    ledger.complete_apply(generation,
                          grappling_hook::CooldownApplyOutcome::targetUnavailable);
    break;
case grappling_hook::CooldownGatewayStatus::layoutUnavailable:
case grappling_hook::CooldownGatewayStatus::verificationFailed:
    ledger.complete_apply(generation, grappling_hook::CooldownApplyOutcome::terminalFailure);
    break;
}
```

Do not call `gateway.apply()` until `pal_game::get_main_container()` returns a valid object. Do not call
it again on later ticks unless an explicit retry has been authorized.

- [ ] **Step 2: Preserve strict gateway behavior and rollback**

Review `grapple_cooldown_gateway.cpp` and keep the following invariants in one call:

- collect only `PalWeaponBase` candidates;
- resolve `ownItemID.StaticId` for every candidate;
- accept only `is_grappling_item_id(rawId)`;
- preflight all required properties before the first write;
- store `{objectFullName, originalCooldown}` before changing any target;
- write `0.1F`;
- immediately read back every changed target;
- on any partial failure, restore every already changed object before returning
  `layoutUnavailable` or `verificationFailed`;
- return `targetUnavailable` only when no matching live grapple object exists.

No timer, cached object pointer, or new hook belongs in the gateway.

- [ ] **Step 3: Add the explicit GUI retry control**

Show the button only when the current snapshot phase is `waitingForRetry`:

```cpp
if (grapplePhase == grappling_hook::CooldownRuntimePhase::waitingForRetry &&
    ImGui::Button(u8"重试应用")) {
    grappleRetryRequested_.store(true, std::memory_order_release);
}
```

Status text must distinguish:

- 等待进入可访问世界；
- 等待本地玩家与 Common 背包就绪；
- 正在应用；
- 已启用；
- 未发现已加载的爪钩枪，可装备后点击“重试应用”；
- 本世界安全禁用；
- 正在恢复；
- 已关闭。

The ImGui callback must not call `get_main_container()` itself.

- [ ] **Step 4: Verify LoadMap ordering**

In `begin_world_transition`:

1. restore the active grapple ledger through the gateway;
2. if restoration fails, keep the old generation blocked and log one warning;
3. only after successful restoration call `ledger.begin_world(newGeneration)`;
4. do not change `grappleRequested_`.

In the post-LoadMap ready path:

- mark the world readable using the existing `worldSession_`;
- call `ledger.begin_world(currentGeneration)` only if not already begun;
- allow the next EngineTick readiness check to schedule one apply.

This ordering guarantees that a process-local desired-on setting is reapplied in a new world but no old
object record crosses generations.

- [ ] **Step 5: Build and run focused tests**

```powershell
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor `
  PalworldEditorBaseResourceSharingTests
ctest --test-dir build -R PalworldEditorBaseResourceSharingTests --output-on-failure
git diff --check
```

Expected: build and focused test pass.

- [ ] **Step 6: Commit the readiness-gated runtime**

```powershell
git add mods/PalworldEditor/src/dllmain.cpp `
  mods/PalworldEditor/inc/grappling_hook/cooldown_gateway.hpp `
  mods/PalworldEditor/src/grapple_cooldown_gateway.cpp
git commit -m "fix: gate grapple override on player readiness"
```

---

## Task 4: Update Documentation for Dynamic Process-Local Controls

**Files:**

- Modify: `README.md`
- Modify: `AGENTS.md`
- Modify: `CLAUDE.md`

- [ ] **Step 1: Remove all persistence claims**

Update the resource and grapple usage sections so they say:

- default off on every game launch;
- checkbox changes take effect dynamically during the current process;
- desired state survives world transitions only within that process;
- no `config.ini` is read or written;
- turning off restores active mutations and unregisters resource hooks.

Remove the deleted settings files from architecture trees.

- [ ] **Step 2: Document the grapple retry contract**

Document:

- automatic one-shot apply after the world and Common inventory are ready;
- no repeated background weapon scan;
- `targetUnavailable` displays a retry button;
- layout/read-back failure safety-disables the feature for that world;
- off and LoadMap restore original cooldowns.

- [ ] **Step 3: Keep the resource caveat explicit**

State that this commit only changes the resource switch lifecycle; the duplicate-count and foreground
operation fixes are implemented by
`docs/superpowers/plans/2026-07-29-resource-sharing-correctness-and-performance.md`.

- [ ] **Step 4: Verify documentation consistency**

```powershell
rg -n "config\\.ini|editor_settings|settings\\.hpp|持久化|写入配置" `
  README.md AGENTS.md CLAUDE.md mods/PalworldEditor
git diff --check
```

Expected: no stale statement saying these two feature switches persist to disk. References to unrelated
configuration must be reviewed rather than blindly removed.

- [ ] **Step 5: Commit documentation**

```powershell
git add README.md AGENTS.md CLAUDE.md
git commit -m "docs: describe dynamic runtime feature controls"
```

---

## Task 5: Full Verification and In-Game Safety Matrix

**Files:**

- Verify only; modify production code only if a test exposes a defect.

- [ ] **Step 1: Run the complete repository verification**

From an x64 VS 2022 developer environment:

```powershell
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor `
  PalworldEditorTests PalworldEditorBaseResourceSharingTests
ctest --test-dir build --output-on-failure
git diff --check
```

Expected: all commands pass.

- [ ] **Step 2: Prove the old settings path is gone**

```powershell
rg -n "editor_settings|load_settings|save_settings|configPath_|BaseResourceSharing.*Enabled|NoCooldown" `
  mods/PalworldEditor README.md AGENTS.md CLAUDE.md
```

Expected: no INI parser/persistence path or persisted-value documentation remains.

- [ ] **Step 3: Deploy**

```powershell
cmake --build --preset ninja-msvc-x64 --target deploy
```

Expected: `main.dll` and `enabled.txt` are refreshed under the configured Palworld installation.

- [ ] **Step 4: Cold-start safety test**

Repeat at least five full desktop cold starts:

1. launch the game with both controls untouched;
2. reach the main menu;
3. confirm no crash and no grapple/resource scan log;
4. enter a save;
5. idle for at least ten seconds;
6. confirm no repeated grapple scan or resource work.

Pass condition: both checkboxes are off every process, startup never applies either feature, and there is
no repeatable frame-time difference from the feature-off baseline.

- [ ] **Step 5: Grapple apply and restore test**

In a save with a grapple gun equipped:

1. enable no-cooldown once;
2. do not toggle it again;
3. verify the status becomes active after Common inventory readiness;
4. fire repeatedly and verify no-cooldown behavior;
5. disable it and verify the original cooldown returns;
6. re-enable and verify one apply;
7. exit and re-enter the world without restarting the process;
8. verify it reapplies after readiness without an off/on toggle;
9. restart the game and verify the checkbox is off.

Pass condition: one scan per authorized apply, exact target behavior, and successful restoration.

- [ ] **Step 6: Target-unavailable retry test**

1. enter a save without a loaded/equipped grapple gun;
2. enable no-cooldown;
3. confirm `waitingForRetry` and no per-frame repeated scan;
4. equip/load a grapple gun;
5. click “重试应用” once;
6. confirm active;
7. wait ten minutes and inspect logs.

Pass condition: no repeated scan/log spam before the click and exactly one retry afterward.

- [ ] **Step 7: LoadMap failure-safety test**

With the override active, repeatedly exit the world and re-enter:

- confirm restoration occurs before the generation changes;
- confirm no stale object full name is reused without re-resolution;
- confirm any restoration verification failure safety-disables instead of applying in the new world;
- confirm no crash in main-menu transitions.

- [ ] **Step 8: Record evidence for the follow-up plan**

Capture:

- the successful build/CTest output;
- one apply, one restore, and one target-unavailable/retry log sequence;
- a feature-off frame-time sample;
- the exact game version and UE4SS experimental build.

Do not mark the combined feature work complete yet. Continue with
`2026-07-29-resource-sharing-correctness-and-performance.md`.

---

## Plan Self-Review Checklist

- [ ] Every persisted settings type, source file, CMake reference, GUI save call, and documentation claim has an explicit removal step.
- [ ] `targetUnavailable` is retryable, while reflection-layout and read-back failures are terminal for the current world.
- [ ] The plan authorizes no per-frame or timer-driven `FindAllOf` scan.
- [ ] The readiness gate uses the existing `pal_game::get_main_container()` and does not retain the pointer.
- [ ] Desired state survives LoadMap only as a pure process-local value; restoration records never cross world generations.
- [ ] Every Unreal operation is game-thread-only and every GUI operation is pure-value-only.
- [ ] RED/GREEN commands, expected failures, and commit boundaries are specified.
- [ ] No `TODO`, `TBD`, “similar to”, placeholder Hook path, or unverified equipment-change API appears in the implementation steps.
