# Event-Driven Pal Target Validation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove confirmed-target background scans while retaining immediate GUID validation for every selection and skill edit.

**Architecture:** Replace the clock-based `PalResolutionScheduler` with a stateless pure event decision. `dllmain.cpp` requests Unreal target resolution only when consuming an explicit selection or edit request; the existing world-generation, target-generation, and freshly resolved GUID safety gates remain unchanged.

**Tech Stack:** C++23, UE4SS C++ API, CMake/Ninja, the repository's dependency-free `CHECK` test harness.

## Global Constraints

- Target runtime/UI/log version is exactly `1.6.2`.
- Do not cache any `UObject*`, Holder, Pal parameter, Unreal array address, or reflection result across EngineTick callbacks.
- Do not add a thread, per-frame scan, timer-driven scan, or new Hook.
- Selection and every edit request must still call `resolve_selected_otomo()` immediately.
- An edit must still match current world generation, selected-target generation, and freshly resolved GUID before writing.
- Do not modify skill-catalog loading, localization, inventory behavior, or base-resource sharing behavior.
- Do not stage or modify `docs/superpowers/plans/2026-07-26-extensible-material-operation-sessions.md`.

---

### Task 1: Replace the time-based scheduler with a pure event decision

**Files:**
- Modify: `mods/PalworldEditor/tests/skill_editor_tests.cpp:589-641`
- Modify: `mods/PalworldEditor/inc/skills/pal_resolution_scheduler.hpp:1-75`

**Interfaces:**
- Produces:
  - `skill_editor::decide_pal_resolution(bool selectionRequested, bool editRequested) noexcept -> PalResolutionTrigger`
- Preserves:
  - `skill_editor::PalResolutionTrigger::{none, selectionRequest, editRequest}`
  - `skill_editor::TargetResolutionSnapshot`
  - `skill_editor::TargetResolutionState`

- [ ] **Step 1: Replace the scheduler tests with failing event-driven tests**

Replace the four `test_pal_resolution_scheduler_*` functions with:

```cpp
void test_pal_resolution_decision_has_zero_idle_work_after_selection() {
    CHECK(skill_editor::decide_pal_resolution(false, false) ==
          skill_editor::PalResolutionTrigger::none);
    CHECK(skill_editor::decide_pal_resolution(true, false) ==
          skill_editor::PalResolutionTrigger::selectionRequest);
    CHECK(skill_editor::decide_pal_resolution(false, false) ==
          skill_editor::PalResolutionTrigger::none);
    CHECK(skill_editor::decide_pal_resolution(false, false) ==
          skill_editor::PalResolutionTrigger::none);
}

void test_pal_resolution_decision_runs_edits_immediately() {
    CHECK(skill_editor::decide_pal_resolution(false, true) ==
          skill_editor::PalResolutionTrigger::editRequest);
}

void test_pal_resolution_decision_prioritizes_selection() {
    CHECK(skill_editor::decide_pal_resolution(true, true) ==
          skill_editor::PalResolutionTrigger::selectionRequest);
}
```

Update `main()` to call exactly these three new functions instead of the four removed scheduler tests.

- [ ] **Step 2: Run the focused test build and verify RED**

Run:

```powershell
cmd.exe /d /c "`"C:\Program Files\Microsoft Visual Studio\18\Enterprise\Common7\Tools\VsDevCmd.bat`" -arch=x64 -host_arch=x64 && cmake --build --preset ninja-msvc-x64 --target PalworldEditorTests"
```

Expected: compilation fails because `decide_pal_resolution` does not exist. The failure must be in the new test, not an unrelated syntax or encoding error.

- [ ] **Step 3: Implement the minimal stateless decision**

In `pal_resolution_scheduler.hpp`:

1. Remove `<chrono>` and `<optional>` if they are no longer needed.
2. Remove `kTargetValidationInterval`.
3. Remove `PalResolutionTrigger::validation`.
4. Remove `PalResolutionScheduler`.
5. Add:

```cpp
[[nodiscard]] constexpr auto decide_pal_resolution(const bool selectionRequested,
                                                    const bool editRequested) noexcept
    -> PalResolutionTrigger {
    if (selectionRequested) {
        return PalResolutionTrigger::selectionRequest;
    }
    if (editRequested) {
        return PalResolutionTrigger::editRequest;
    }
    return PalResolutionTrigger::none;
}
```

Keep `TargetResolutionSnapshot` and `TargetResolutionState` unchanged.

- [ ] **Step 4: Run the focused tests and verify GREEN**

Run:

```powershell
cmd.exe /d /c "`"C:\Program Files\Microsoft Visual Studio\18\Enterprise\Common7\Tools\VsDevCmd.bat`" -arch=x64 -host_arch=x64 && cmake --build --preset ninja-msvc-x64 --target PalworldEditorTests && ctest --test-dir build -R PalworldEditor.SkillEditor --output-on-failure"
```

Expected: build exits zero and `PalworldEditor.SkillEditor` passes.

- [ ] **Step 5: Commit the pure behavior**

```powershell
git add mods/PalworldEditor/inc/skills/pal_resolution_scheduler.hpp mods/PalworldEditor/tests/skill_editor_tests.cpp
git commit -m "perf: make Pal target validation event driven"
```

---

### Task 2: Integrate event-driven resolution into EngineTick

**Files:**
- Modify: `mods/PalworldEditor/src/dllmain.cpp:263-269`
- Modify: `mods/PalworldEditor/src/dllmain.cpp:424-425`
- Modify: `mods/PalworldEditor/src/dllmain.cpp:467`
- Modify: `mods/PalworldEditor/src/dllmain.cpp:897-901`
- Modify: `mods/PalworldEditor/src/dllmain.cpp:1082`

**Interfaces:**
- Consumes:
  - `skill_editor::decide_pal_resolution(bool selectionRequested, bool editRequested) noexcept`
- Preserves:
  - `apply_if_target_is_current(...)`
  - `WorldSessionState::request_is_current(...)`
  - `SelectedTargetState::matches(...)`

- [ ] **Step 1: Replace the EngineTick scheduler call**

Replace:

```cpp
const auto trigger = palResolutionScheduler_.decide(
    worldSession_.is_target_confirmed() && selectedTarget_.is_selected(),
    selectionRequested, editRequest.has_value(),
    skill_editor::PalResolutionScheduler::clock::now());
```

with:

```cpp
const auto trigger =
    skill_editor::decide_pal_resolution(selectionRequested, editRequest.has_value());
```

Keep the existing `if (trigger != PalResolutionTrigger::none)` block, which performs the immediate
fresh resolution used by selection and edit safety checks.

- [ ] **Step 2: Remove obsolete state and lifecycle resets**

Remove:

```cpp
palResolutionScheduler_.reset();
```

from both world-transition paths, and remove the `PalResolutionScheduler palResolutionScheduler_;`
member. Do not change `targetResolutionState_.reset()`.

- [ ] **Step 3: Update the GUI behavior comment**

Change the `render_pal_editor` details to state:

- idle confirmed targets do not run background resolution;
- selection and edit requests resolve immediately;
- only another explicit selection changes the locked target.

- [ ] **Step 4: Build the production DLL**

Run:

```powershell
cmd.exe /d /c "`"C:\Program Files\Microsoft Visual Studio\18\Enterprise\Common7\Tools\VsDevCmd.bat`" -arch=x64 -host_arch=x64 && cmake --build --preset ninja-msvc-x64 --target PalworldEditor"
```

Expected: `PalworldEditor.dll` builds successfully with no reference to `PalResolutionScheduler`,
`kTargetValidationInterval`, or `PalResolutionTrigger::validation`.

- [ ] **Step 5: Verify the runtime path statically**

Run:

```powershell
rg -n "PalResolutionScheduler|kTargetValidationInterval|PalResolutionTrigger::validation|250.*校验|250.*validation" mods/PalworldEditor
rg -n -C 8 "resolve_selected_otomo" mods/PalworldEditor/src/dllmain.cpp
```

Expected:

- the first command returns no matches;
- the second shows that `resolve_selected_otomo()` is guarded only by a trigger derived from
  `selectionRequested` or `editRequest.has_value()`.

- [ ] **Step 6: Commit the EngineTick integration**

```powershell
git add mods/PalworldEditor/src/dllmain.cpp
git commit -m "perf: stop idle Pal target scans"
```

---

### Task 3: Release 1.6.2 and run the final verification gate

**Files:**
- Modify: `mods/PalworldEditor/src/dllmain.cpp:56-67`
- Modify: `README.md`
- Modify: `AGENTS.md`
- Modify: `CLAUDE.md`

**Interfaces:**
- Produces runtime metadata, load log, and GUI title `1.6.2`.
- Documents the event-driven target-validation contract and manual performance check.

- [ ] **Step 1: Update version strings**

Set:

```cpp
ModVersion = STR("1.6.2");
Output::send<LogLevel::Verbose>(STR("PalworldEditor loaded (v1.6.2)\n"));
ImGui::Begin("PalworldEditor v1.6.2", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
```

- [ ] **Step 2: Update documentation**

Document these exact points in `README.md`, `AGENTS.md`, and `CLAUDE.md`:

- selected targets are value locks and are not polled while idle;
- selection and every edit still perform immediate fresh resolution and GUID validation;
- number-key changes never silently switch the locked target;
- 1.6.2 removes the former 250 ms `FindAllOf` path;
- actual frame-time improvement requires in-game validation.

Do not describe the paused material-operation-session plan as implemented.

- [ ] **Step 3: Run the complete repository gate**

Run:

```powershell
cmd.exe /d /c "`"C:\Program Files\Microsoft Visual Studio\18\Enterprise\Common7\Tools\VsDevCmd.bat`" -arch=x64 -host_arch=x64 && cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor PalworldEditorTests PalworldEditorBaseResourceSharingTests && ctest --test-dir build --output-on-failure"
git diff --check
```

Expected:

- format check succeeds;
- `PalworldEditor.dll` builds;
- both CTest tests pass with zero failures;
- `git diff --check` exits zero.

- [ ] **Step 4: Recheck scope and produce the artifact hash**

Run:

```powershell
git status --short
git diff --stat HEAD
Get-FileHash -Algorithm SHA256 -LiteralPath build/Game__Shipping__Win64/bin/PalworldEditor.dll
```

Expected:

- only files in this plan are changed;
- the paused untracked material-operation plan remains unmodified and untracked;
- a SHA-256 hash is reported for the new DLL.

- [ ] **Step 5: Commit the release metadata and docs**

```powershell
git add mods/PalworldEditor/src/dllmain.cpp README.md AGENTS.md CLAUDE.md
git commit -m "docs: release PalworldEditor 1.6.2"
```

- [ ] **Step 6: Preserve the branch without deployment**

Keep branch `codex/fix-next-summon-pal` as-is. Do not deploy, push, merge, or create a pull request
unless the user asks separately.
