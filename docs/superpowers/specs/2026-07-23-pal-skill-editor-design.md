# Selected Pal Skill Editor Design

**Date:** 2026-07-23

**Status:** Approved for implementation planning

**Target release:** MyPalMod 1.4.0

## Goal

Extend MyPalMod so the currently selected Pal can be edited through the UE4SS ImGui window:

- list, add, replace, and remove individual passive skills;
- list, equip, replace, and clear the three currently equipped active-skill slots;
- choose new skills from searchable runtime catalogs containing localized display names and raw IDs.

The feature is intended for single-player games and the host/local server. Ordinary multiplayer clients are not supported.

## Terminology

- **Passive skill:** an `FName` stored in the individual Pal's passive-skill list.
- **Active skill:** an `EPalWazaID` stored in the individual Pal's `EquipWaza` list.
- **Mastered active skill:** an entry in `MasteredWaza`. This design does not modify the mastered-skill list.
- **Partner Skill:** a species-level skill defined by `CharacterID`. Partner Skills are outside this feature.
- **Automatic target:** the Pal most recently observed by the existing `GetPassiveSkillList` hook.
- **Manual target:** the valid Pal selected from the existing `Scan Pals` list.

## Scope

### Included

- Automatic-target-first selection, with the manually selected Pal as fallback.
- A catalog of Pal-assignable passive skills.
- A catalog of all valid `EPalWazaID` active skills.
- Localized skill names with raw ID fallback.
- Search by localized name or raw ID.
- Passive-skill add, replace, and remove operations.
- Active-skill equip, replace, and clear operations for three slots.
- Serialized game-thread execution, result refresh, user-visible status, and replacement rollback.
- Automated tests for catalog and edit-control logic.
- Documentation and version metadata updated consistently to `1.4.0`.

### Excluded

- Modifying `MasteredWaza`.
- Modifying species-level Partner Skills.
- Editing DataTables, save arrays, or property memory directly.
- Unpacking game assets or maintaining an offline static skill database.
- Supporting ordinary multiplayer clients.
- Changing unrelated inventory behavior.

## Architecture

The implementation is divided into a pure C++ control layer, a Palworld runtime gateway, and the existing ImGui/application layer.

### `skill_catalog.hpp`

A standard-library-only component containing:

```cpp
struct SkillOption
{
    std::string id;
    std::string displayName;
    std::string searchText;
};
```

It owns pure functions for:

- normalizing localized names and IDs for search;
- sorting and deduplicating options;
- filtering by the search string;
- excluding skills already owned or equipped;
- determining whether another passive skill may be added.

It has no Unreal or ImGui dependency and is directly unit-testable.

### `skill_editor_service.hpp`

A pure control component that consumes a gateway interface and implements edit semantics:

```cpp
using SkillTarget = std::uintptr_t;

enum class SkillKind
{
    passive,
    active,
};

enum class SkillEditOperation
{
    add,
    replace,
    remove,
};

struct SkillEditRequest
{
    SkillTarget target;
    SkillKind kind;
    SkillEditOperation operation;
    std::string oldId;
    std::string newId;
};
```

`SkillTarget` is the captured pointer value treated as an opaque token. Only the production Palworld gateway converts it back to a non-owning `UObject*`; the pure service and tests never depend on Unreal types. The service is responsible for validation, ordered execution, refresh requests, and replacement rollback. Tests use an in-memory fake gateway.

### `pal_skills.hpp`

The Palworld/UE4SS gateway is responsible for:

- locating the passive-skill manager and UI utility default object;
- obtaining Pal-assignable passive IDs through a correctly constructed Unreal `TArray<FName>`;
- enumerating `EPalWazaID` through the runtime `UEnum`;
- resolving localized passive and active skill names;
- reading `GetPassiveSkillList()` and `GetEquipWaza()`;
- calling `AddPassiveSkill`, `RemovePassiveSkill`, `AddEquipWaza`, and `RemoveEquipWaza`;
- converting Unreal values into standard-library values before they cross into the GUI cache.

It must use real RE-UE4SS `TArray`, `FName`, `FText`, and enum facilities. It must not represent an Unreal array as an unmanaged `{Data, Num, Max}` struct.

### `dllmain.cpp`

The mod class remains responsible for:

- resolving the current target;
- owning GUI state and thread-safe caches;
- rendering the skill editor;
- staging `SkillEditRequest` values;
- draining the request queue from `on_update()` on the game thread;
- displaying the latest operation result.

Unreal reflection and `ProcessEvent` calls remain confined to the game thread.

## Target Resolution

The selected target is resolved in this order:

1. Use the automatic target when the pointer is valid.
2. Otherwise use the valid manual selection from the scanned Pal list.
3. Otherwise disable all edit controls and display `No valid Pal selected`.

Each request captures the target selected at click time. Switching the displayed Pal after submission does not redirect the queued request to the new target.

Immediately before execution, the gateway validates the captured target. An invalid target cancels the request without modifying another Pal.

The UI labels the source:

```text
Target: SheepBall [currently viewed]
```

or:

```text
Target: SheepBall [manual selection]
```

## Skill Catalogs

### Passive skills

The passive catalog contains only skills returned by:

```cpp
UPalPassiveSkillManager::GetPalAssignablePassiveIDs(TArray<FName>&)
```

It does not enumerate every row in `PassiveSkillDataTable`, because that would include equipment, hidden, boss, test, or system-only passives.

Display names come from:

```cpp
UPalUIUtility::GetPassiveSkillName(WorldContext, PassiveSkillId, OutName)
```

If localization fails, the raw `FName` ID is used for both display and search.

### Active skills

The active catalog enumerates the runtime `EPalWazaID` `UEnum`.

The catalog excludes only invalid sentinels such as `None` and `MAX`. All other runtime enum values remain available. Entries without a localized name show the raw enum ID.

Display names come from:

```cpp
UPalUIUtility::GetWazaName(WorldContext, WazaId, OutName)
```

### Loading and refresh

Catalogs are loaded on the game thread after Unreal initialization and cached as immutable `std::vector<SkillOption>` values for the GUI thread.

If a required manager, enum, default object, or world context is unavailable, loading reports an error but keeps the last successful cache. A `Refresh skill catalogs` button requests another load.

## Current Skill State

For the selected target, the gateway reads:

- passive skills through `GetPassiveSkillList()`;
- equipped active skills through `GetEquipWaza()`.

The GUI always renders three active slots. Missing array entries are rendered as empty slots. The feature does not infer or modify `MasteredWaza`.

After every edit request, the gateway reads both lists again. UI state is based on the returned game state rather than the requested state.

`EquipWaza` is treated as an ordered list rather than a random-access setter API. Replacing an active slot reads the complete original list, builds the desired list in memory, calls `ClearEquipWaza()`, and then calls `AddEquipWaza()` for each desired entry in order. This preserves the positions of unaffected skills without directly writing the array.

## UI Design

The existing duplicate "currently viewed" and manually selected passive controls are replaced by one unified editor beneath the resolved target banner.

### Passive section

```text
Passive Skills  3/4

1. Rare [Rare]                     [Replace...] [Remove]
2. Powerful [Powerful]             [Replace...] [Remove]
3. Artisan [CraftSpeed_up2]        [Replace...] [Remove]

[Search/select passive...] [Add]
```

Rules:

- The UI allows at most four passive skills.
- At four skills, Add is disabled; Replace and Remove remain enabled.
- Skills already owned are excluded from Add and Replace candidates.
- Choosing a dropdown entry does not edit the Pal until Add or Replace is clicked.

### Active section

```text
Active Skills

Slot 1: Fire Ball [FireBall]       [Replace...] [Clear]
Slot 2: Wind Cutter [WindCutter]   [Replace...] [Clear]
Slot 3: Empty                      [Select...]  [Equip]
```

Rules:

- Exactly three slots are shown.
- Skills equipped in another slot are excluded from the candidate list.
- Replace removes the old equipped skill and adds the new one.
- Equip adds a skill to an empty slot.
- Clear removes the slot's current skill.
- The mastered-skill list is neither displayed nor modified.

### Searchable dropdown

Each selector displays:

```text
Localized Name [RawId]
```

Search matches localized names and IDs case-insensitively. A missing localized name renders only `RawId`.

Edit buttons are disabled while an edit request for the selected target is pending.

## Request Queue and Threading

The GUI thread pushes complete `SkillEditRequest` values into a mutex-protected FIFO queue. It never calls Unreal APIs.

`on_update()` drains requests in order on the game thread. Multiple rapid clicks cannot overwrite earlier requests.

Catalog and current-skill snapshots are copied into mutex-protected GUI caches only after Unreal values have been converted to standard-library values. The GUI does not retain references into Unreal arrays.

## Edit Semantics

### Add

1. Validate target and new ID.
2. Confirm the skill is not already present.
3. Confirm passive count is below four, or that an active slot is empty.
4. Call the corresponding game API.
5. Refresh current state and report whether the new ID is present.

### Remove

1. Validate target and old ID.
2. Confirm the old ID is currently present.
3. Call the corresponding removal API.
4. Refresh and report whether the old ID disappeared.

### Replace

1. Validate target, old ID, and new ID.
2. Confirm the old ID is present and the new ID is absent.
3. For a passive skill, remove the old ID and add the new ID.
4. For an active skill, build the desired ordered three-slot list, clear the equipped list, and re-add the desired entries in order.
5. Refresh current state.
6. If the resulting state does not match the desired state, restore the complete original passive or active state and refresh again.
7. Report success, rollback success, or rollback failure.

The operation status is retained in the UI until another request or catalog refresh replaces it.

## Error Handling

Expected user-facing statuses include:

- `Replaced Rare with Powerful`
- `Removed FireBall from active slot 1`
- `Target Pal is no longer valid; operation cancelled`
- `Skill catalog is not loaded`
- `Replacement failed; original skill restored`
- `Replacement and rollback both failed; refresh the Pal state`

Detailed function lookup, target, old ID, new ID, and post-refresh state are written to the UE4SS log.

Failures do not clear a previously successful catalog cache.

## Persistence and Multiplayer

The design uses Palworld's individual-character mutation functions instead of directly editing reflected arrays. In single-player and on the host, those functions are expected to update the individual save parameter and relevant delegates.

Persistence is verified by saving, leaving, and reloading the world during end-to-end testing. Ordinary multiplayer clients are explicitly unsupported because server authority may reject or overwrite local changes.

## Automated Testing

The repository gains a lightweight CTest executable that depends only on the pure catalog and service components.

Tests cover:

- localized-name search;
- raw-ID search;
- case-insensitive matching;
- option sorting and deduplication;
- excluding owned or already equipped skills;
- passive add disabled at four skills;
- rendering/normalizing exactly three active slots;
- preserving unaffected active-slot order during replacement;
- successful add, remove, and replace flows;
- replacement failure followed by successful rollback;
- replacement and rollback failure reporting;
- invalid-target cancellation;
- FIFO processing of multiple requests.

Tests use an in-memory fake gateway. Runtime `ProcessEvent` marshalling remains an integration boundary verified by compilation and in-game testing.

## End-to-End Acceptance

1. Build MyPalMod with the `ninja-msvc-x64` preset without new warnings.
2. Deploy it to a Palworld installation with UE4SS Experimental.
3. Open a Pal detail page and confirm automatic target selection.
4. Confirm the passive catalog contains Pal-assignable entries with localized names and IDs.
5. Confirm the active catalog contains valid runtime `EPalWazaID` entries.
6. Add, replace, and remove passive skills.
7. Equip, replace, and clear each active slot.
8. Confirm a fourth passive disables Add while Replace and Remove remain available.
9. Switch the viewed Pal immediately after submitting an edit and confirm the request does not affect the new target.
10. Save and reload the world and confirm the edits persist.
11. Confirm UE4SS logs contain no array corruption, invalid-object access, or `ProcessEvent` parameter errors.
12. Confirm `ModVersion`, load log, window title, README, and release description all report `1.4.0`.
