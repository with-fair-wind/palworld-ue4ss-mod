# Base Resource Toggle Reactivation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Restore base-resource discovery and sharing when the user turns the feature off and back on without leaving the current world.

**Architecture:** A pure C++ transition function decides whether a toggle is unchanged, disables the runtime, or starts the currently accessible world. The Unreal bridge uses that decision to reinitialize the existing lease and reconcile scheduler with the current generation; catalog discovery remains deferred to the existing game-thread tick.

**Tech Stack:** C++23, CMake 3.22+, Ninja/MSVC, RE-UE4SS experimental, standard-library-only policy tests, CTest.

## Global Constraints

- Keep the broader early build/craft eligibility plan paused.
- Support only Palworld 1.0 single-player and local listen-server host.
- Do not access Unreal from the ImGui callback or UE4SS UpdateThread.
- Do not retain raw Unreal pointers across callbacks.
- Do not add threads, global UObject scans, item-slot reads, or new periodic work.
- Re-enable only schedules the existing manager-based catalog reconciliation.
- Repeated requests for the already active state must not restart discovery.
- A world-level safety disable caused by failed union restoration cannot be bypassed by toggling.
- Keep existing 1-second failure backoff and 8-second idle fallback behavior unchanged.
- Implement the pure transition behavior with an observed RED test before production code.
- Target runtime/UI/log version is exactly `1.6.1`.
- Do not deploy to the game directory unless the user separately requests deployment.

## File Structure

- Modify `mods/PalworldEditor/inc/base_resource_sharing/resource_session.hpp`: add the pure toggle-transition value and decision function.
- Modify `mods/PalworldEditor/tests/base_resource_sharing_tests.cpp`: test all toggle transitions and scheduler restart after reset.
- Modify `mods/PalworldEditor/src/pal_base_resources.cpp`: apply the transition and restart the current accessible world state.
- Modify `mods/PalworldEditor/src/dllmain.cpp`: publish version `1.6.1`.
- Modify `README.md`, `AGENTS.md`, and `CLAUDE.md`: document same-world off/on behavior and its performance contract.

---

### Task 1: Add a Tested Toggle-Lifecycle Decision

**Files:**
- Modify: `mods/PalworldEditor/inc/base_resource_sharing/resource_session.hpp`
- Modify: `mods/PalworldEditor/tests/base_resource_sharing_tests.cpp`

**Interfaces:**
- Produces:
  - `struct ResourceToggleTransition`
  - `decide_resource_toggle(bool wasEnabled, bool requestedEnabled, bool worldAccessible) noexcept -> ResourceToggleTransition`
- `ResourceToggleTransition` fields:
  - `bool disableRuntime`
  - `bool beginAccessibleWorld`

- [ ] **Step 1: Write the failing toggle-transition test**

Add to `base_resource_sharing_tests.cpp`:

```cpp
void test_resource_toggle_transition_distinguishes_disable_and_accessible_reenable() {
    using namespace base_resource_sharing;

    auto transition = decide_resource_toggle(false, false, true);
    CHECK(!transition.disableRuntime);
    CHECK(!transition.beginAccessibleWorld);

    transition = decide_resource_toggle(true, true, true);
    CHECK(!transition.disableRuntime);
    CHECK(!transition.beginAccessibleWorld);

    transition = decide_resource_toggle(true, false, true);
    CHECK(transition.disableRuntime);
    CHECK(!transition.beginAccessibleWorld);

    transition = decide_resource_toggle(false, true, false);
    CHECK(!transition.disableRuntime);
    CHECK(!transition.beginAccessibleWorld);

    transition = decide_resource_toggle(false, true, true);
    CHECK(!transition.disableRuntime);
    CHECK(transition.beginAccessibleWorld);
}
```

Register it in `main()`.

- [ ] **Step 2: Extend the scheduler test with reset/re-enable behavior**

Append to `test_reconcile_scheduler_coalesces_events_and_uses_bounded_intervals()`:

```cpp
scheduler.reset();
CHECK(!scheduler.advance(0.0F, 7));
scheduler.begin_world(7);
CHECK(scheduler.advance(0.0F, 7));
scheduler.complete(true, 7);
CHECK(!scheduler.advance(0.0F, 7));
```

This expresses the exact broken sequence: disable resets the scheduler, and same-world re-enable
must begin it again.

- [ ] **Step 3: Run the focused test target and observe RED**

Run:

```powershell
cmd.exe /d /c "`"C:\Program Files\Microsoft Visual Studio\18\Enterprise\Common7\Tools\VsDevCmd.bat`" -arch=x64 -host_arch=x64 && cmake --build --preset ninja-msvc-x64 --target PalworldEditorBaseResourceSharingTests"
```

Expected: compilation fails because `ResourceToggleTransition` and
`decide_resource_toggle` do not exist.

- [ ] **Step 4: Implement the minimal pure decision**

Add to `resource_session.hpp`:

```cpp
struct ResourceToggleTransition {
    bool disableRuntime{};
    bool beginAccessibleWorld{};
};

[[nodiscard]] constexpr auto decide_resource_toggle(const bool wasEnabled,
                                                    const bool requestedEnabled,
                                                    const bool worldAccessible) noexcept
    -> ResourceToggleTransition {
    if (wasEnabled == requestedEnabled) {
        return {};
    }
    if (!requestedEnabled) {
        return {.disableRuntime = true};
    }
    return {.beginAccessibleWorld = worldAccessible};
}
```

Do not add stored state, dynamic allocation, Unreal dependencies, or side effects.

- [ ] **Step 5: Run the focused test and observe GREEN**

Run:

```powershell
cmd.exe /d /c "`"C:\Program Files\Microsoft Visual Studio\18\Enterprise\Common7\Tools\VsDevCmd.bat`" -arch=x64 -host_arch=x64 && cmake --build --preset ninja-msvc-x64 --target PalworldEditorBaseResourceSharingTests && ctest --test-dir build -R PalworldEditor.BaseResourceSharing --output-on-failure"
```

Expected: the resource-sharing test target builds and its CTest passes.

- [ ] **Step 6: Commit the tested policy**

```powershell
git add mods/PalworldEditor/inc/base_resource_sharing/resource_session.hpp mods/PalworldEditor/tests/base_resource_sharing_tests.cpp
git commit -m "test: define base resource toggle transitions"
```

---

### Task 2: Restart the Current World Runtime on Re-enable

**Files:**
- Modify: `mods/PalworldEditor/src/pal_base_resources.cpp`

**Interfaces:**
- Consumes:
  - `decide_resource_toggle`
  - `RuntimeState::enabled()`
  - `RuntimeState::accessible()`
  - `RuntimeState::generation()`
  - `ResourceUnionLeaseState::begin_world`
  - `ReconcileScheduler::begin_world`

- [ ] **Step 1: Establish the GREEN policy baseline**

Run:

```powershell
cmd.exe /d /c "`"C:\Program Files\Microsoft Visual Studio\18\Enterprise\Common7\Tools\VsDevCmd.bat`" -arch=x64 -host_arch=x64 && cmake --build --preset ninja-msvc-x64 --target PalworldEditorBaseResourceSharingTests && ctest --test-dir build -R PalworldEditor.BaseResourceSharing --output-on-failure"
```

Expected: the transition and scheduler restart tests pass before wiring the Unreal bridge.

- [ ] **Step 2: Apply the transition in `set_enabled`**

Replace the current conditional with:

```cpp
auto set_enabled(const bool enabled) -> void {
    const auto transition =
        decide_resource_toggle(runtime_.enabled(), enabled, runtime_.accessible());
    if (!transition.disableRuntime && !transition.beginAccessibleWorld &&
        runtime_.enabled() == enabled) {
        return;
    }

    if (transition.disableRuntime) {
        leases_.reset();
        scheduler_.reset();
        restore_or_disable("关闭资源共享");
        unregister_resource_hooks();
        catalog_ = {};
        baseCount_ = 0;
        containerCount_ = 0;
        worldContextFullName_.clear();
        runtimeError_.clear();
    }

    runtime_.set_preference(enabled);
    if (transition.beginAccessibleWorld) {
        const auto generation = runtime_.generation();
        leases_.begin_world(generation);
        scheduler_.begin_world(generation);
        runtimeError_.clear();
    }
    snapshotDirty_.mark();
    publish_snapshot();
}
```

Do not clear `worldDisabledErrors_` on re-enable. A restoration failure must remain disabled for
the current world.

- [ ] **Step 3: Verify the EngineTick ordering remains safe**

Inspect `PalworldEditorMod::game_thread_tick` and keep this order:

```cpp
baseResourceBridge_.set_enabled(requestedBaseSharingEnabled_.load());
baseResourceBridge_.ensure_hooks_registered();
baseResourceBridge_.tick(deltaSeconds);
```

Do not invoke discovery directly from `set_enabled`. `scheduler_.begin_world()` marks one pending
calibration, and the existing `tick()` performs it on the game thread.

- [ ] **Step 4: Build all targets and run CTest**

Run:

```powershell
cmd.exe /d /c "`"C:\Program Files\Microsoft Visual Studio\18\Enterprise\Common7\Tools\VsDevCmd.bat`" -arch=x64 -host_arch=x64 && cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor PalworldEditorTests PalworldEditorBaseResourceSharingTests && ctest --test-dir build --output-on-failure"
```

Expected: format check and all builds succeed; both CTests pass.

- [ ] **Step 5: Verify the change adds no scan or recurring path**

Run:

```powershell
git diff -- mods/PalworldEditor/src/pal_base_resources.cpp
rg -n "FindAllOf|ItemSlotArray|StackCount|std::thread|CreateThread" mods/PalworldEditor/src/pal_base_resources.cpp mods/PalworldEditor/src/pal_base_resource_runtime.cpp
```

Expected:

- the bridge diff only adds pure transition handling and two `begin_world` calls;
- the prohibited-pattern search returns no matches;
- no discovery, `ProcessEvent`, object lookup, or log call is added to `set_enabled`.

- [ ] **Step 6: Commit the lifecycle fix**

```powershell
git add mods/PalworldEditor/src/pal_base_resources.cpp
git commit -m "fix: reactivate base resources after toggle"
```

---

### Task 3: Release 1.6.1 and Run the Final Verification Gate

**Files:**
- Modify: `mods/PalworldEditor/src/dllmain.cpp`
- Modify: `README.md`
- Modify: `AGENTS.md`
- Modify: `CLAUDE.md`

**Interfaces:**
- Produces:
  - runtime metadata, load log, and GUI title `1.6.1`
  - documentation of same-world off/on behavior and performance constraints

- [ ] **Step 1: Update exact version strings**

In `dllmain.cpp`:

```cpp
ModVersion = STR("1.6.1");
Output::send<LogLevel::Verbose>(STR("PalworldEditor loaded (v1.6.1)\n"));
ImGui::Begin("PalworldEditor v1.6.1", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
```

- [ ] **Step 2: Update documentation**

In `README.md`, `AGENTS.md`, and `CLAUDE.md`:

- change the current version and expected load message from `1.6.0` to `1.6.1`;
- state that closing sharing restores and clears the resource runtime;
- state that reopening it in an accessible world restarts the scheduler with the current
  generation and automatically rediscovers resources;
- state that reopening adds no global scan, slot scan, thread, or per-frame task;
- add a same-world off/on regression check to the manual validation section;
- state that the broader early build/craft eligibility redesign remains outside this patch.

Do not edit historical specs or plans.

- [ ] **Step 3: Verify versions and stale current-contract text**

Run:

```powershell
rg -n "1\.6\.0|重新开启.*0|关闭.*重进" README.md AGENTS.md CLAUDE.md mods/PalworldEditor/src/dllmain.cpp
```

Expected: no stale current-version or “must re-enter the world” contract remains.

Run:

```powershell
rg -n "1\.6\.1|重新开启|当前世界代次|逐帧" README.md AGENTS.md CLAUDE.md mods/PalworldEditor/src/dllmain.cpp
```

Expected: version strings and reactivation/performance contracts are present.

- [ ] **Step 4: Run complete local verification**

Run:

```powershell
cmd.exe /d /c "`"C:\Program Files\Microsoft Visual Studio\18\Enterprise\Common7\Tools\VsDevCmd.bat`" -arch=x64 -host_arch=x64 && cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor PalworldEditorTests PalworldEditorBaseResourceSharingTests && ctest --test-dir build --output-on-failure"
git diff --check
```

Expected: all targets build, both CTests pass, and `git diff --check` is silent.

- [ ] **Step 5: Commit release metadata and documentation**

```powershell
git add mods/PalworldEditor/src/dllmain.cpp README.md AGENTS.md CLAUDE.md
git commit -m "docs: release PalworldEditor 1.6.1"
```

- [ ] **Step 6: Verify branch state and artifact without deploying**

Run:

```powershell
git status --short --branch
git log -5 --oneline
Get-FileHash -Algorithm SHA256 'build/Game__Shipping__Win64/bin/PalworldEditor.dll'
```

Expected:

- production and test changes are committed;
- the intentionally paused
  `docs/superpowers/plans/2026-07-26-extensible-material-operation-sessions.md` remains untracked;
- the local DLL hash is recorded;
- no game installation file has been changed.

Game-side validation after a separately requested deployment:

1. Enter a single-player/local-host world and enable sharing.
2. Wait for nonzero resource-base and container counts.
3. Disable sharing and confirm counts become 0.
4. Without leaving the world, enable sharing again.
5. Confirm counts automatically return without re-entering the save.
6. Repeat off/on three times; each enable produces one successful catalog reconciliation.
7. Remain in build and factory menus for 2–3 minutes; confirm no new recurring logs or perceptible
   frame-time spikes.
