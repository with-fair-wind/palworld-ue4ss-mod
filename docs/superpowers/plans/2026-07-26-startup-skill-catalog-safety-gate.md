# Startup Skill Catalog Safety Gate Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prevent PalworldEditor from calling skill catalog and localization reflection before the local player's Common inventory container is available.

**Architecture:** Extend the pure skill-catalog retry scheduler with a lazily evaluated runtime-readiness predicate. `dllmain.cpp` supplies a game-thread predicate that resolves and immediately validates the Common inventory container only when a refresh is due; `PalSkillGateway::load_catalog()` is never called when that predicate fails.

**Tech Stack:** C++23, UE4SS Unreal reflection, CMake/Ninja/MSVC, existing pure C++ skill-editor tests.

## Global Constraints

- Runtime readiness is checked only when a manual or automatic catalog refresh is due, never on every EngineTick.
- A missing Common inventory container defers the refresh by the existing 2-second retry interval.
- Manual refresh bypasses time throttling but never bypasses runtime readiness.
- No `UObject*`, Unreal array address, or property address survives the current EngineTick callback.
- Existing catalog section fallback and localization behavior remain unchanged after runtime readiness.
- Pal resolution scheduling, dirty snapshot publication, and base-resource sharing are out of scope.
- Runtime, GUI, and startup version becomes exactly `1.5.3`.
- Do not deploy or push; preserve branch `codex/fix-next-summon-pal`.

---

### Task 1: Runtime-Gated Skill Catalog Refresh

**Files:**
- Modify: `mods/PalworldEditor/inc/skills/skill_catalog.hpp`
- Modify: `mods/PalworldEditor/tests/skill_editor_tests.cpp`
- Modify: `mods/PalworldEditor/src/dllmain.cpp`

**Interfaces:**
- Consumes: `pal_game::get_main_container()` and `pal_game::is_valid(UObject*)`.
- Produces:
  - `SkillCatalogRefreshScheduler::should_refresh(bool manual, bool ready, time_point now, RuntimeReady&& runtimeReady) -> bool`
  - a lazy runtime predicate that is not called while automatic retries are throttled or the catalog is already ready.

- [ ] **Step 1: Add the failing runtime-gate regression test**

Add this test beside the existing catalog scheduler tests:

```cpp
void test_catalog_refresh_scheduler_defers_unsafe_runtime_queries() {
    using namespace std::chrono_literals;
    skill_editor::SkillCatalogRefreshScheduler scheduler{2s};
    const auto start = skill_editor::SkillCatalogRefreshScheduler::time_point{};
    auto readinessChecks = 0;

    CHECK(!scheduler.should_refresh(false, false, start, [&readinessChecks] {
        ++readinessChecks;
        return false;
    }));
    CHECK(readinessChecks == 1);
    CHECK(!scheduler.should_refresh(false, false, start + 1s, [&readinessChecks] {
        ++readinessChecks;
        return true;
    }));
    CHECK(readinessChecks == 1);
    CHECK(scheduler.should_refresh(false, false, start + 2s, [&readinessChecks] {
        ++readinessChecks;
        return true;
    }));
    CHECK(readinessChecks == 2);
}
```

Register it in `main()`. Do not change production code yet.

- [ ] **Step 2: Run the focused test target and verify RED**

Run in the VS x64 developer environment:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorTests
```

Expected: compilation fails because `SkillCatalogRefreshScheduler::should_refresh` does not accept the fourth runtime-readiness argument.

- [ ] **Step 3: Implement the lazy runtime-readiness gate**

Replace the three-argument scheduler method with:

```cpp
template <typename RuntimeReady>
[[nodiscard]] auto should_refresh(bool manual, bool ready, time_point now,
                                  RuntimeReady&& runtimeReady) -> bool {
    if (!manual && (ready || (hasAttempted_ && now < nextAutomaticRefresh_))) {
        return false;
    }

    hasAttempted_ = true;
    nextAutomaticRefresh_ = now + retryInterval_;
    return std::forward<RuntimeReady>(runtimeReady)();
}
```

Document that manual refresh skips time throttling but still invokes the safety predicate. The existing `<utility>` include supplies `std::forward`.

- [ ] **Step 4: Update existing scheduler tests to provide an always-ready predicate**

Change each existing three-argument call in
`test_catalog_refresh_scheduler_throttles_automatic_retries()` and
`test_catalog_refresh_scheduler_honors_manual_refresh_immediately()` to pass:

```cpp
[] { return true; }
```

Add explicit coverage for both remaining branches:

```cpp
void test_catalog_refresh_scheduler_never_bypasses_runtime_gate() {
    using namespace std::chrono_literals;
    skill_editor::SkillCatalogRefreshScheduler scheduler{2s};
    const auto start = skill_editor::SkillCatalogRefreshScheduler::time_point{};
    auto readinessChecks = 0;

    CHECK(!scheduler.should_refresh(true, false, start, [&readinessChecks] {
        ++readinessChecks;
        return false;
    }));
    CHECK(readinessChecks == 1);
    CHECK(scheduler.should_refresh(true, false, start + 1ms, [&readinessChecks] {
        ++readinessChecks;
        return true;
    }));
    CHECK(readinessChecks == 2);
    CHECK(!scheduler.should_refresh(false, true, start + 2s, [&readinessChecks] {
        ++readinessChecks;
        return true;
    }));
    CHECK(readinessChecks == 2);
}
```

Register the test in `main()`.

- [ ] **Step 5: Verify the pure scheduler is GREEN**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorTests
ctest --test-dir build -R PalworldEditor.SkillEditor --output-on-failure
```

Expected: the target builds and the focused CTest reports `1/1` passed.

- [ ] **Step 6: Integrate the Common inventory safety predicate**

Change the `game_thread_tick()` catalog scheduler call to:

```cpp
const bool refreshRequested = skillCatalogRefreshScheduler_.should_refresh(
    manualRefreshRequested, catalogReady,
    skill_editor::SkillCatalogRefreshScheduler::clock::now(), [] {
        return pal_game::is_valid(pal_game::get_main_container());
    });
```

The returned container is tested and discarded inside the same lambda. Do not store it in a local outside the predicate or in a class member. Leave `load_catalog()`, catalog fallback, LoadMap handling, Pal resolution, and resource sharing unchanged.

- [ ] **Step 7: Format, build, and run all tests**

Run sequentially in the VS x64 developer environment:

```powershell
cmake --build --preset ninja-msvc-x64 --target format
cmake --build --preset ninja-msvc-x64 --target PalworldEditor PalworldEditorTests PalworldEditorBaseResourceSharingTests
ctest --test-dir build --output-on-failure
git diff --check
```

Expected: the DLL links, both CTest cases pass, and `git diff --check` is silent. The existing third-party PatternSleuth unused-import warning is allowed.

- [ ] **Step 8: Inspect scope and commit the safety gate**

Run:

```powershell
git diff --stat
git diff -- mods/PalworldEditor/inc/skills/skill_catalog.hpp mods/PalworldEditor/tests/skill_editor_tests.cpp mods/PalworldEditor/src/dllmain.cpp
git diff --name-only
```

Verify no base-resource or Pal-resolution file changed, then commit:

```powershell
git add mods/PalworldEditor/inc/skills/skill_catalog.hpp mods/PalworldEditor/tests/skill_editor_tests.cpp mods/PalworldEditor/src/dllmain.cpp
git commit -m "fix: gate startup skill catalog reflection"
```

---

### Task 2: Release PalworldEditor 1.5.3

**Files:**
- Modify: `mods/PalworldEditor/src/dllmain.cpp`
- Modify: `README.md`
- Modify: `AGENTS.md`

**Interfaces:**
- Consumes: the runtime safety gate from Task 1.
- Produces: exact `1.5.3` runtime/UI/log metadata and game-side validation instructions.

- [ ] **Step 1: Update exact runtime version strings**

Change the three `1.5.2` strings in `dllmain.cpp`:

```cpp
ModVersion = STR("1.5.3");
Output::send<LogLevel::Verbose>(STR("PalworldEditor loaded (v1.5.3)\n"));
ImGui::Begin("PalworldEditor v1.5.3", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
```

- [ ] **Step 2: Document the startup safety gate**

Update `README.md` and `AGENTS.md` to state:

- version `1.5.3`;
- skill catalog reflection waits for a valid local Common inventory container;
- readiness is checked only when the existing 2-second refresh is due;
- manual refresh cannot bypass the safety gate;
- the 1.5.2 Pal performance behavior and base-resource logic are unchanged;
- game-side validation should include repeated cold starts before entering the main menu.

- [ ] **Step 3: Run fresh complete verification**

Run in the VS x64 developer environment:

```powershell
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor PalworldEditorTests PalworldEditorBaseResourceSharingTests
ctest --test-dir build --output-on-failure
git diff --check
```

Expected: format check and DLL build exit 0, both CTest cases pass, and diff check is silent.

- [ ] **Step 4: Inspect final evidence and commit release metadata**

Run:

```powershell
git diff --stat
git status --short
git branch --show-current
```

Verify only `AGENTS.md`, `README.md`, and `dllmain.cpp` remain for this commit, then:

```powershell
git add AGENTS.md README.md mods/PalworldEditor/src/dllmain.cpp
git commit -m "docs: release PalworldEditor 1.5.3"
```

- [ ] **Step 5: Preserve the branch without deployment**

Run:

```powershell
git status --short
git branch --show-current
git log --oneline -6
```

Expected: clean `codex/fix-next-summon-pal`. Do not run `deploy` and do not push.
