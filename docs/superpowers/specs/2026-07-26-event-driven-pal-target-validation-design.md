# Event-Driven Pal Target Validation Design

## Context

PalworldEditor currently resolves the number-key-highlighted party Pal immediately when the user
selects a target or submits an edit. After a target has been confirmed, it also repeats the same
resolution every 250 milliseconds.

The resolution begins with:

```cpp
UObjectGlobals::FindAllOf(STR("PalOtomoHolderComponentBase"), holders);
```

UE4SS implements `FindAllOf` by iterating the global Unreal object array and checking each object's
class hierarchy. Running that work on the game thread four times per second can create periodic
frame-time spikes. Because selecting a target is a prerequisite for editing a passive skill, the
spikes can appear to begin after the first edit even though the edit request itself is finite.

Passive add/remove/replace requests are already one-shot FIFO operations. A successful request does
not remain queued and does not trigger a skill-catalog refresh. The persistent cost comes from the
target-validation schedule, not from a persistent passive-edit task.

## Goal

PalworldEditor 1.6.2 will resolve the current party Pal only for explicit selection and edit
requests. An idle confirmed target must cause no periodic holder discovery or target reflection.

## Non-Goals

- Cache `UObject*` handles across frames.
- Automatically switch the locked edit target when the user presses a number key.
- Change passive or active skill write semantics.
- Change skill-catalog loading, localization, base-resource sharing, or inventory behavior.
- Promise a specific FPS improvement without an in-game frame-time measurement.

## Considered Approaches

### 1. Event-driven resolution at selection and edit time

Resolve once when “选择当前帕鲁” is clicked. Preserve only the existing value identity
(`FPalInstanceID.InstanceId`, name, and generation). Resolve again immediately before each edit and
require the current GUID to match the locked identity.

This removes all idle scans while preserving the write safety check. It is the selected approach.

### 2. Cache the local Holder `UObject*`

This would avoid repeated global discovery while retaining background validation. However, a
non-owning Unreal object can become invalid during world transitions or object replacement. Safely
managing that lifetime would add hooks and invalidation complexity, contrary to the repository's
rule that scanned Pal-related objects are valid only for the current callback.

### 3. Increase the validation interval

Changing 250 milliseconds to a longer interval would reduce scan frequency but leave a recurring
game-thread global scan. It would also make the GUI's target-match display stale for an arbitrary
duration. This treats the symptom instead of removing the unnecessary work.

## Runtime Design

The resolution trigger becomes a pure event decision:

- selection request present: `selectionRequest`;
- otherwise, edit request present: `editRequest`;
- otherwise: `none`.

There is no clock, next-validation deadline, or validation trigger.

### Selection

1. The GUI submits a world-bound selection request.
2. The next EngineTick resolves the highlighted party Pal.
3. A successful result stores only its value identity and confirms the target for the current world.
4. The gateway reads the Pal's current skills once for the GUI snapshot.

### Idle confirmed target

No Pal resolution occurs. The GUI continues to display the explicitly locked target from the
published value snapshot. Pressing another number key does not silently switch the edit target and
does not start work.

The last successful `targetMatchesCurrent` value is therefore a statement about the most recent
explicit resolution, not a live polling signal.

### Edit

1. The GUI queues an edit request containing the target generation and world generation.
2. The next EngineTick resolves the currently highlighted party Pal immediately.
3. The existing safety gate checks:
   - the request belongs to the accessible current world;
   - the request target generation still matches;
   - the freshly resolved GUID matches the locked target GUID.
4. Only then does the gateway execute the passive or active edit.

If the player has changed the number-key-highlighted Pal or resolution is temporarily unavailable,
the edit is rejected. The locked target remains selected; the user can explicitly select again.

### World transitions

LoadMap behavior is unchanged. Pending requests are cleared, write access is revoked, and the target
must be explicitly confirmed in the new world. No Unreal object pointer is retained.

## Components and Files

- `inc/skills/pal_resolution_scheduler.hpp`
  - Replace the time-based scheduler with a stateless event decision.
  - Remove the 250 ms interval and `validation` trigger.
  - Keep `TargetResolutionState`, because it stores only the latest value snapshot.
- `src/dllmain.cpp`
  - Request resolution only for selection or edit events.
  - Remove scheduler reset/member state.
  - Update comments that currently promise 250 ms background validation.
- `tests/skill_editor_tests.cpp`
  - Prove repeated idle decisions never request resolution.
  - Prove selection and edit requests resolve immediately.
  - Preserve selection priority if both events are present.
- `README.md`, `AGENTS.md`, `CLAUDE.md`
  - Publish version 1.6.2 and document the event-driven safety/performance contract.

## Error Handling and Safety

- A failed selection publishes the existing detailed resolution status and does not confirm a
  target.
- A failed or mismatched edit resolution rejects the request and clears stale queued edits exactly
  as today.
- The implementation does not weaken world-generation or target-generation validation.
- No `UObject*`, Holder, Pal parameter, array address, or reflection result escapes its EngineTick.

## Test Strategy

Pure C++ regression tests will verify:

1. idle decisions return `none`, including repeated calls after a selection event;
2. a selection request returns `selectionRequest`;
3. an edit request returns `editRequest`;
4. selection retains priority over edit when both are presented;
5. existing target-GUID and world-generation rejection tests remain green.

Repository verification will run:

```powershell
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor PalworldEditorTests PalworldEditorBaseResourceSharingTests
ctest --test-dir build --output-on-failure
git diff --check
```

Static review will confirm that the EngineTick has no clock-driven call to
`resolve_selected_otomo()`. Actual frame-time improvement still requires an in-game comparison:
select a Pal without editing, wait at least ten seconds, then repeat an edit while monitoring frame
times.

## Acceptance Criteria

- A confirmed target left idle does not call `resolve_selected_otomo()`.
- Selection and every edit still perform an immediate fresh resolution.
- Switching the highlighted Pal and then editing is rejected before any skill write.
- The locked target changes only after another explicit selection.
- No new per-frame task, background thread, global scan, or cross-frame Unreal pointer is added.
- All repository build, format, and test gates pass.
