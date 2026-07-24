# Skill Catalog Crash and Localization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Eliminate the skill-catalog refresh crash, make passive and active pickers independently usable, and display skill names in the game's current language.

**Architecture:** Replace direct `UEnum` storage reads with a generated, checked-in Palworld 1.0 `{value, Raw ID}` table and use `PalUIUtility` only for display-name localization. Split the catalog snapshot into independent passive and active sections so a failure in one section cannot disable or erase the other.

**Tech Stack:** C++23, UE4SS experimental runtime, Palworld 1.0 UHT dump, CMake/Ninja/MSVC, standard-library-only unit tests, PowerShell generation script.

## Global Constraints

- Target PalworldEditor version is exactly `1.4.4`.
- Skill display names must follow the game's current language; do not hard-code Simplified Chinese or English names.
- The active-skill table contains only numeric values and Raw IDs from `UHTHeaderDump/Pal/Public/EPalWazaID.h`.
- Do not call `UEnum::GetEnumNames()` or read `UEnum` member storage.
- All Unreal reflection calls remain on the `on_update()` game thread.
- Do not retain `UObject*` values across frames or pass them to the GUI thread.
- Passive and active catalog availability, errors, fallback, and picker enablement remain independent.
- A localization failure falls back to Raw IDs and does not disable editing.
- Existing selected-target generation checks, stale-request rejection, and skill write/rollback behavior remain unchanged.
- Use TDD for pure C++ behavior and run the repository's complete verification commands before completion.

---

## File Structure

- Create `scripts/generate-active-skill-definitions.ps1`: deterministically convert the checked-in UHT enum into a C++ value/Raw-ID table.
- Create `mods/PalworldEditor/inc/skills/active_skill_definitions.hpp`: generated, Unreal-independent active-skill definitions and numeric lookup helpers.
- Modify `mods/PalworldEditor/inc/skills/skill_catalog.hpp`: build active options from definitions and represent passive/active catalog sections independently.
- Modify `mods/PalworldEditor/tests/skill_editor_tests.cpp`: cover generated mappings, localization fallback, independent section fallback, and partial availability.
- Modify `mods/PalworldEditor/inc/skills/pal_skills.hpp`: remove the runtime-populated active-ID cache and document static lookup behavior.
- Modify `mods/PalworldEditor/src/pal_skills.cpp`: remove `UEnum` enumeration, localize through `PalPlayerInventoryData`, and populate independent sections.
- Modify `mods/PalworldEditor/src/dllmain.cpp`: bind each picker and error message to its own catalog section and update v1.4.4 metadata.
- Modify `README.md`, `AGENTS.md`, and `CLAUDE.md`: document v1.4.4 and the safe/localized catalog architecture.

---

### Task 1: Deterministic Active-Skill Definitions

**Files:**
- Create: `scripts/generate-active-skill-definitions.ps1`
- Create: `mods/PalworldEditor/inc/skills/active_skill_definitions.hpp`
- Modify: `mods/PalworldEditor/inc/skills/skill_catalog.hpp`
- Test: `mods/PalworldEditor/tests/skill_editor_tests.cpp`

**Interfaces:**
- Consumes: `UHTHeaderDump/Pal/Public/EPalWazaID.h`.
- Produces: `skill_editor::ActiveSkillDefinition`, `active_skill_definitions()`, `find_active_skill_id(std::uint16_t)`, `active_skill_id_or_numeric(std::uint16_t)`, and `make_active_skill_options(std::span<const ActiveSkillDefinition>, Localizer)`.

- [ ] **Step 1: Write failing mapping and option-construction tests**

Add the generated-header include:

```cpp
#include <optional>
#include <string_view>

#include <skills/active_skill_definitions.hpp>
```

Add these tests before `main()`:

```cpp
void test_active_skill_definitions_are_unique_and_known_values_match() {
    const auto definitions = skill_editor::active_skill_definitions();
    CHECK(!definitions.empty());

    std::unordered_set<std::uint16_t> values;
    std::unordered_set<std::string_view> ids;
    for (const auto& definition : definitions) {
        CHECK(definition.value != 0);
        CHECK(!definition.id.empty());
        CHECK(definition.id != "None");
        CHECK(definition.id != "MAX");
        CHECK(values.insert(definition.value).second);
        CHECK(ids.insert(definition.id).second);
    }

    CHECK(skill_editor::find_active_skill_id(1) ==
          std::optional<std::string_view>{"Human_Punch"});
    CHECK(skill_editor::find_active_skill_id(15) ==
          std::optional<std::string_view>{"Unique_Boar_Tackle"});
    CHECK(skill_editor::find_active_skill_id(22) ==
          std::optional<std::string_view>{"AirCanon"});
    CHECK(skill_editor::find_active_skill_id(124) ==
          std::optional<std::string_view>{"MudShot"});
    CHECK(!skill_editor::find_active_skill_id(0).has_value());
    CHECK(skill_editor::active_skill_id_or_numeric(65535) == "65535");
}

void test_active_skill_options_use_runtime_localization_with_raw_id_fallback() {
    constexpr std::array definitions{
        skill_editor::ActiveSkillDefinition{.value = 15, .id = "Unique_Boar_Tackle"},
        skill_editor::ActiveSkillDefinition{.value = 124, .id = "MudShot"},
    };

    const auto options = skill_editor::make_active_skill_options(
        definitions, [](const skill_editor::ActiveSkillDefinition& definition) {
            return definition.value == 15 ? std::string{"野猪突进"} : std::string{};
        });

    CHECK(options.size() == 2);
    CHECK(options[0].id == "Unique_Boar_Tackle");
    CHECK(options[0].localizedName == "野猪突进");
    CHECK(options[0].activeValue == std::optional<std::uint16_t>{std::uint16_t{15}});
    CHECK(skill_editor::skill_label(options[0]) == "野猪突进 [Unique_Boar_Tackle]");
    CHECK(options[1].id == "MudShot");
    CHECK(options[1].localizedName.empty());
    CHECK(skill_editor::skill_label(options[1]) == "MudShot");
}
```

Invoke both tests from `main()`:

```cpp
test_active_skill_definitions_are_unique_and_known_values_match();
test_active_skill_options_use_runtime_localization_with_raw_id_fallback();
```

- [ ] **Step 2: Run the test target and verify the missing interface failure**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorTests
```

Expected: compilation fails because `skills/active_skill_definitions.hpp` and the new lookup interfaces do not exist.

- [ ] **Step 3: Add the deterministic UHT-to-header generator**

Create `scripts/generate-active-skill-definitions.ps1` with this complete behavior:

```powershell
param(
    [string]$InputPath = "UHTHeaderDump/Pal/Public/EPalWazaID.h",
    [string]$OutputPath = "mods/PalworldEditor/inc/skills/active_skill_definitions.hpp"
)

$resolvedInput = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\$InputPath"))
$resolvedOutput = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\$OutputPath"))
$entries = [Collections.Generic.List[object]]::new()
$nextValue = 0

foreach ($line in [IO.File]::ReadAllLines($resolvedInput)) {
    if ($line -notmatch '^\s*([A-Za-z_][A-Za-z0-9_]*)(?:\s*=\s*(\d+))?,\s*$') {
        continue
    }

    $name = $Matches[1]
    if ($Matches[2]) {
        $nextValue = [int]$Matches[2]
    }
    if ($nextValue -gt [uint16]::MaxValue) {
        throw "EPalWazaID value $nextValue for $name exceeds uint16"
    }
    if ($name -notin @("None", "MAX")) {
        $entries.Add([pscustomobject]@{ Name = $name; Value = $nextValue })
    }
    ++$nextValue
}

if ($entries.Count -eq 0) {
    throw "No EPalWazaID entries were parsed from $resolvedInput"
}
if (($entries.Value | Sort-Object -Unique).Count -ne $entries.Count) {
    throw "EPalWazaID contains duplicate non-sentinel numeric values"
}
if (($entries.Name | Sort-Object -Unique).Count -ne $entries.Count) {
    throw "EPalWazaID contains duplicate non-sentinel names"
}

$lines = [Collections.Generic.List[string]]::new()
$lines.Add("/**")
$lines.Add(" * @file active_skill_definitions.hpp")
$lines.Add(" * @brief Generated Palworld 1.0 active-skill values and Raw IDs.")
$lines.Add(" * @note Generated by scripts/generate-active-skill-definitions.ps1 from")
$lines.Add(" *       UHTHeaderDump/Pal/Public/EPalWazaID.h; do not edit manually.")
$lines.Add(" */")
$lines.Add("#pragma once")
$lines.Add("")
$lines.Add("#include <array>")
$lines.Add("#include <cstdint>")
$lines.Add("#include <optional>")
$lines.Add("#include <span>")
$lines.Add("#include <string>")
$lines.Add("#include <string_view>")
$lines.Add("")
$lines.Add("namespace skill_editor {")
$lines.Add("struct ActiveSkillDefinition {")
$lines.Add("    std::uint16_t value;")
$lines.Add("    std::string_view id;")
$lines.Add("};")
$lines.Add("")
$lines.Add("inline constexpr std::array kActiveSkillDefinitions{")
foreach ($entry in $entries) {
    $lines.Add(
        "    ActiveSkillDefinition{.value = $($entry.Value), .id = `"$($entry.Name)`"},")
}
$lines.Add("};")
$lines.Add("")
$lines.Add("[[nodiscard]] constexpr auto active_skill_definitions() noexcept")
$lines.Add("    -> std::span<const ActiveSkillDefinition> {")
$lines.Add("    return kActiveSkillDefinitions;")
$lines.Add("}")
$lines.Add("")
$lines.Add("[[nodiscard]] constexpr auto find_active_skill_id(const std::uint16_t value) noexcept")
$lines.Add("    -> std::optional<std::string_view> {")
$lines.Add("    for (const auto& definition : kActiveSkillDefinitions) {")
$lines.Add("        if (definition.value == value) {")
$lines.Add("            return definition.id;")
$lines.Add("        }")
$lines.Add("    }")
$lines.Add("    return std::nullopt;")
$lines.Add("}")
$lines.Add("")
$lines.Add("[[nodiscard]] inline auto active_skill_id_or_numeric(const std::uint16_t value) -> std::string {")
$lines.Add("    if (const auto id = find_active_skill_id(value)) {")
$lines.Add("        return std::string(*id);")
$lines.Add("    }")
$lines.Add("    return std::to_string(value);")
$lines.Add("}")
$lines.Add("}  // namespace skill_editor")

$utf8WithoutBom = [Text.UTF8Encoding]::new($false)
[IO.File]::WriteAllText($resolvedOutput, [string]::Join("`n", $lines) + "`n", $utf8WithoutBom)
Write-Host "Generated $($entries.Count) active skills at $resolvedOutput"
```

- [ ] **Step 4: Generate the checked-in C++ header**

Run from the repository root:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/generate-active-skill-definitions.ps1
```

Expected: prints `Generated <non-zero count> active skills` and creates
`mods/PalworldEditor/inc/skills/active_skill_definitions.hpp`. The generated table must map
`15` to `Unique_Boar_Tackle` and `124` to `MudShot`.

- [ ] **Step 5: Add the pure active-option constructor**

In `skill_catalog.hpp`, include the generated definitions and add the template after
`SkillOption`:

```cpp
#include <skills/active_skill_definitions.hpp>

template <typename Localizer>
[[nodiscard]] auto make_active_skill_options(
    const std::span<const ActiveSkillDefinition> definitions, Localizer&& localize)
    -> std::vector<SkillOption> {
    std::vector<SkillOption> options;
    options.reserve(definitions.size());
    for (const auto& definition : definitions) {
        options.push_back({
            .id = std::string(definition.id),
            .localizedName = localize(definition),
            .activeValue = definition.value,
        });
    }
    return options;
}
```

Keep `<utility>` included because the catalog helpers use `std::move`.

- [ ] **Step 6: Build and run the tests**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorTests
ctest --test-dir build --output-on-failure
```

Expected: the test executable builds and `PalworldEditor.SkillEditor` passes.

- [ ] **Step 7: Verify deterministic regeneration**

Run:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/generate-active-skill-definitions.ps1
git diff --check
git status --short
```

Expected: the second generation does not change the generated header; `git diff --check` exits
successfully.

- [ ] **Step 8: Commit the active-skill source**

```powershell
git add scripts/generate-active-skill-definitions.ps1 mods/PalworldEditor/inc/skills/active_skill_definitions.hpp mods/PalworldEditor/inc/skills/skill_catalog.hpp mods/PalworldEditor/tests/skill_editor_tests.cpp
git commit -m "feat: add deterministic active skill definitions"
```

---

### Task 2: Independent Passive and Active Catalog State

**Files:**
- Modify: `mods/PalworldEditor/inc/skills/skill_catalog.hpp`
- Modify: `mods/PalworldEditor/tests/skill_editor_tests.cpp`
- Modify: `mods/PalworldEditor/src/pal_skills.cpp`
- Modify: `mods/PalworldEditor/src/dllmain.cpp`
- Modify: `mods/PalworldEditor/inc/skills/pal_skills.hpp`

**Interfaces:**
- Consumes: existing `SkillOption` lists and refresh results.
- Produces: `SkillCatalogSection`, `SkillCatalogSnapshot::passive`,
  `SkillCatalogSnapshot::active`, and section-by-section `with_catalog_fallback()`.

- [ ] **Step 1: Replace the all-or-nothing fallback test with independent-section tests**

Replace `test_skill_catalog_refresh_keeps_last_success()` with:

```cpp
void test_skill_catalog_refresh_merges_sections_independently() {
    const skill_editor::SkillCatalogSnapshot previous{
        .passive = {
            .skills = {{.id = "Passive_Old", .localizedName = "旧被动"}},
            .ready = true,
        },
        .active = {
            .skills = {{.id = "OldActive", .activeValue = std::uint16_t{1}}},
            .ready = true,
        },
    };
    const skill_editor::SkillCatalogSnapshot refreshed{
        .passive = {
            .skills = {{.id = "Passive_New", .localizedName = "新被动"}},
            .ready = true,
        },
        .active = {.error = "active refresh failed"},
    };

    const auto merged = skill_editor::with_catalog_fallback(previous, refreshed);
    CHECK(merged.passive.ready);
    CHECK(merged.passive.skills.size() == 1);
    CHECK(merged.passive.skills[0].id == "Passive_New");
    CHECK(merged.passive.error.empty());
    CHECK(merged.active.ready);
    CHECK(merged.active.skills.size() == 1);
    CHECK(merged.active.skills[0].id == "OldActive");
    CHECK(merged.active.error == "active refresh failed");
}

void test_skill_catalog_first_partial_load_keeps_available_section() {
    const skill_editor::SkillCatalogSnapshot refreshed{
        .passive = {
            .skills = {{.id = "Passive_Swift", .localizedName = "神速"}},
            .ready = true,
        },
        .active = {.error = "active unavailable"},
    };

    const auto merged = skill_editor::with_catalog_fallback({}, refreshed);
    CHECK(merged.passive.ready);
    CHECK(merged.passive.skills.size() == 1);
    CHECK(!merged.active.ready);
    CHECK(merged.active.skills.empty());
    CHECK(merged.active.error == "active unavailable");
}
```

Update the `main()` calls:

```cpp
test_skill_catalog_refresh_merges_sections_independently();
test_skill_catalog_first_partial_load_keeps_available_section();
```

- [ ] **Step 2: Run tests and verify the old snapshot shape fails**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorTests
```

Expected: compilation fails because `SkillCatalogSnapshot` has no `passive` or `active` members.

- [ ] **Step 3: Implement section state and independent fallback**

Replace the current `SkillCatalogSnapshot` and `with_catalog_fallback()` definitions in
`skill_catalog.hpp` with:

```cpp
struct SkillCatalogSection {
    std::vector<SkillOption> skills;
    std::string error;
    bool ready{};
};

struct SkillCatalogSnapshot {
    SkillCatalogSection passive;
    SkillCatalogSection active;
};

[[nodiscard]] inline auto with_section_fallback(const SkillCatalogSection& previous,
                                                const SkillCatalogSection& refreshed)
    -> SkillCatalogSection {
    if (refreshed.ready || !previous.ready) {
        return refreshed;
    }

    auto fallback = previous;
    fallback.error = refreshed.error;
    return fallback;
}

[[nodiscard]] inline auto with_catalog_fallback(const SkillCatalogSnapshot& previous,
                                                const SkillCatalogSnapshot& refreshed)
    -> SkillCatalogSnapshot {
    return {
        .passive = with_section_fallback(previous.passive, refreshed.passive),
        .active = with_section_fallback(previous.active, refreshed.active),
    };
}
```

- [ ] **Step 4: Update the runtime gateway to the new shape without changing its source**

In `PalSkillGateway::load_catalog()` keep the existing Holder world context and
`UEnum::GetEnumNames()` temporarily, but migrate field access:

```cpp
catalog.passive.skills.push_back(
    {.id = text_encoding::to_utf8(id.ToString()),
     .localizedName = passive_localized_name(utility, worldContext, id)});
catalog.passive.skills = skill_editor::deduplicate_skills(
    std::move(catalog.passive.skills));
catalog.active.skills.push_back(
    {.id = std::move(id),
     .localizedName =
         active_localized_name(utility, worldContext, static_cast<EPalWazaID>(value)),
     .activeValue = value});
```

Replace final validation and readiness:

```cpp
if (catalog.passive.skills.empty()) {
    catalog.passive.error = "Unable to load Pal-assignable passive skills";
} else {
    std::ranges::sort(catalog.passive.skills, byLabel);
    catalog.passive.ready = true;
}

if (catalog.active.skills.empty()) {
    catalog.active.error = "Unable to load EPalWazaID active skills";
} else {
    std::ranges::sort(catalog.active.skills, byLabel);
    catalog.active.ready = true;
}
```

If the Holder world context is unavailable, set both errors and return:

```cpp
catalog.passive.error = "Local player party Holder world context is unavailable";
catalog.active.error = "Local player party Holder world context is unavailable";
return catalog;
```

Rebuild `activeIds_` from `catalog.active.skills`. Update the `load_catalog()` contract in
`pal_skills.hpp` to say each section reports its own readiness and error.

- [ ] **Step 5: Bind each GUI path to its own section**

In `dllmain.cpp`, replace passive accesses with:

```cpp
snapshot.catalog.passive.skills
snapshot.catalog.passive.ready
```

Replace active accesses with:

```cpp
snapshot.catalog.active.skills
snapshot.catalog.active.ready
```

Render errors independently:

```cpp
if (!snapshot.catalog.passive.error.empty()) {
    ImGui::TextColored(ImVec4(1.0F, 0.45F, 0.35F, 1.0F),
                       "被动技能目录：%s", snapshot.catalog.passive.error.c_str());
}
if (!snapshot.catalog.active.error.empty()) {
    ImGui::TextColored(ImVec4(1.0F, 0.45F, 0.35F, 1.0F),
                       "主动技能目录：%s", snapshot.catalog.active.error.c_str());
}
```

After copying `snapshot` in `render_pal_editor()`, clear stale GUI choices independently:

```cpp
const auto choiceStillExists = [](const std::optional<skill_editor::SkillOption>& choice,
                                  const skill_editor::SkillCatalogSection& section) {
    return !choice.has_value() ||
           std::ranges::any_of(section.skills, [&choice](const auto& option) {
               return option.id == choice->id;
           });
};
if (!choiceStillExists(self->passiveChoice_, snapshot.catalog.passive)) {
    self->passiveChoice_.reset();
}
if (!choiceStillExists(self->activeChoice_, snapshot.catalog.active)) {
    self->activeChoice_.reset();
}
```

- [ ] **Step 6: Build the mod and tests**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditor PalworldEditorTests
ctest --test-dir build --output-on-failure
```

Expected: both targets build and all CTest tests pass. This intermediate commit still has the
old runtime enum read; do not deploy it.

- [ ] **Step 7: Commit the independent state model**

```powershell
git add scripts/generate-active-skill-definitions.ps1 mods/PalworldEditor/inc/skills/active_skill_definitions.hpp mods/PalworldEditor/inc/skills/skill_catalog.hpp mods/PalworldEditor/inc/skills/pal_skills.hpp mods/PalworldEditor/src/pal_skills.cpp mods/PalworldEditor/src/dllmain.cpp mods/PalworldEditor/tests/skill_editor_tests.cpp
git commit -m "refactor: split passive and active catalog state"
```

---

### Task 3: Safe Runtime Loading and Game-Language Localization

**Files:**
- Modify: `mods/PalworldEditor/inc/game/pal_game.hpp`
- Modify: `mods/PalworldEditor/inc/skills/pal_skills.hpp`
- Modify: `mods/PalworldEditor/src/pal_skills.cpp`
- Test: `mods/PalworldEditor/tests/skill_editor_tests.cpp`

**Interfaces:**
- Consumes: `active_skill_definitions()`, `active_skill_id_or_numeric()`,
  `make_active_skill_options()`, and independent catalog sections.
- Produces: a `PalSkillGateway::load_catalog()` implementation with no runtime `UEnum` access and
  localized names obtained from `PalUIUtility`.

- [ ] **Step 1: Add a regression assertion that known equipped values never need a runtime map**

Extend `test_active_skill_definitions_are_unique_and_known_values_match()`:

```cpp
CHECK(skill_editor::active_skill_id_or_numeric(15) == "Unique_Boar_Tackle");
CHECK(skill_editor::active_skill_id_or_numeric(124) == "MudShot");
```

- [ ] **Step 2: Run the focused tests**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorTests
ctest --test-dir build --output-on-failure
```

Expected: tests pass, establishing the replacement lookup before runtime wiring changes.

- [ ] **Step 3: Remove the runtime active-ID cache**

In `pal_skills.hpp`:

- remove `#include <unordered_map>`;
- remove the private `activeIds_` member;
- update `read_state()` documentation to state that equipped values use the generated
  Palworld 1.0 definition table;
- update `load_catalog()` documentation to state that `PalPlayerInventoryData` is used only for
  localization and Raw-ID catalogs remain available if localization is unavailable.

In `read_state()` replace:

```cpp
const auto found = activeIds_.find(value);
state.activeSkills.push_back(
    {.value = value,
     .id = found == activeIds_.end() ? std::to_string(value) : found->second});
```

with:

```cpp
state.activeSkills.push_back(
    {.value = value, .id = skill_editor::active_skill_id_or_numeric(value)});
```

Keep the existing three-slot limit and warning unchanged.

- [ ] **Step 4: Cache localization function lookups per refresh**

Change the two localization helpers to receive an already-resolved `UFunction*`:

```cpp
[[nodiscard]] auto passive_localized_name(UObject* utility, UFunction* function,
                                          UObject* worldContext, const FName& id)
    -> std::string {
    if (utility == nullptr || function == nullptr || worldContext == nullptr) {
        return {};
    }
    struct Params {
        UObject* WorldContextObject;
        FName PassiveSkillId;
        FText OutName;
    } params{.WorldContextObject = worldContext, .PassiveSkillId = id};
    utility->ProcessEvent(function, &params);
    return text_encoding::to_utf8(params.OutName.ToString());
}

[[nodiscard]] auto active_localized_name(UObject* utility, UFunction* function,
                                         UObject* worldContext, const EPalWazaID id)
    -> std::string {
    if (utility == nullptr || function == nullptr || worldContext == nullptr) {
        return {};
    }
    struct Params {
        UObject* WorldContextObject;
        EPalWazaID WazaId;
        FText OutName;
    } params{.WorldContextObject = worldContext, .WazaId = id};
    utility->ProcessEvent(function, &params);
    return text_encoding::to_utf8(params.OutName.ToString());
}
```

Delete `strip_enum_prefix()` and `is_active_sentinel()` because the generated source has already
performed both responsibilities.

- [ ] **Step 5: Replace `load_catalog()` with safe independent loading**

Use this control flow:

```cpp
auto PalSkillGateway::load_catalog() -> skill_editor::SkillCatalogSnapshot {
    skill_editor::SkillCatalogSnapshot catalog;
    auto* const worldContext = UObjectGlobals::FindFirstOf(pal_game::kInventoryClassName);
    auto* const utility = ui_utility();
    auto* const passiveNameFunction = find_function<UFunction>(
        STR("/Script/Pal.PalUIUtility:GetPassiveSkillName"));
    auto* const activeNameFunction =
        find_function<UFunction>(STR("/Script/Pal.PalUIUtility:GetWazaName"));

    auto* const manager = UObjectGlobals::FindFirstOf(STR("PalPassiveSkillManager"));
    auto* const passiveListFunction = find_function<UFunction>(
        STR("/Script/Pal.PalPassiveSkillManager:GetPalAssignablePassiveIDs"));
    if (manager != nullptr && passiveListFunction != nullptr) {
        struct Params {
            TArray<FName> List;
        } params;
        manager->ProcessEvent(passiveListFunction, &params);
        catalog.passive.skills.reserve(
            static_cast<std::size_t>(std::max(params.List.Num(), 0)));
        for (int32 index = 0; index < params.List.Num(); ++index) {
            const auto& id = params.List[index];
            catalog.passive.skills.push_back({
                .id = text_encoding::to_utf8(id.ToString()),
                .localizedName = passive_localized_name(
                    utility, passiveNameFunction, worldContext, id),
            });
        }
        catalog.passive.skills =
            skill_editor::deduplicate_skills(std::move(catalog.passive.skills));
    }

    catalog.active.skills = skill_editor::make_active_skill_options(
        skill_editor::active_skill_definitions(),
        [utility, activeNameFunction, worldContext](
            const skill_editor::ActiveSkillDefinition& definition) {
            return active_localized_name(
                utility, activeNameFunction, worldContext,
                static_cast<EPalWazaID>(definition.value));
        });

    const auto byLabel = [](const skill_editor::SkillOption& left,
                            const skill_editor::SkillOption& right) {
        return skill_editor::ascii_lower(skill_editor::skill_label(left)) <
               skill_editor::ascii_lower(skill_editor::skill_label(right));
    };

    if (catalog.passive.skills.empty()) {
        catalog.passive.error = "Unable to load Pal-assignable passive skills";
    } else {
        std::ranges::sort(catalog.passive.skills, byLabel);
        catalog.passive.ready = true;
    }
    if (catalog.active.skills.empty()) {
        catalog.active.error = "Generated EPalWazaID catalog is empty";
    } else {
        std::ranges::sort(catalog.active.skills, byLabel);
        catalog.active.ready = true;
    }

    const bool passiveHasLocalizedNames =
        std::ranges::any_of(catalog.passive.skills, [](const auto& option) {
            return !option.localizedName.empty();
        });
    const bool activeHasLocalizedNames =
        std::ranges::any_of(catalog.active.skills, [](const auto& option) {
            return !option.localizedName.empty();
        });
    const bool localizationContextReady = utility != nullptr && worldContext != nullptr;
    if (catalog.passive.ready &&
        (!localizationContextReady || passiveNameFunction == nullptr ||
         !passiveHasLocalizedNames)) {
        catalog.passive.error =
            "Skill localization is unavailable; showing Raw IDs until refresh";
    }
    if (catalog.active.ready &&
        (!localizationContextReady || activeNameFunction == nullptr ||
         !activeHasLocalizedNames)) {
        catalog.active.error =
            "Skill localization is unavailable; showing Raw IDs until refresh";
    }
    return catalog;
}
```

Remove the entire `StaticFindObject<UEnum*>`, `GetEnumNames()`, `seenValues`, and `activeIds_`
rebuild blocks. Remove now-unused `<limits>` and `<unordered_set>` includes.

- [ ] **Step 6: Verify the dangerous call is absent**

Run:

```powershell
rg -n "GetEnumNames|StaticFindObject<UEnum|activeIds_|strip_enum_prefix|is_active_sentinel" mods/PalworldEditor
```

Expected: no matches.

- [ ] **Step 7: Build and run all automated tests**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor PalworldEditorTests
ctest --test-dir build --output-on-failure
git diff --check
```

Expected: formatting, mod build, test build, CTest, and whitespace validation all pass.

- [ ] **Step 8: Commit the crash fix and localization path**

```powershell
git add mods/PalworldEditor/inc/game/pal_game.hpp mods/PalworldEditor/inc/skills/pal_skills.hpp mods/PalworldEditor/src/pal_skills.cpp mods/PalworldEditor/tests/skill_editor_tests.cpp
git commit -m "fix: load localized skills without runtime enum reads"
```

---

### Task 4: Version, Documentation, and Release Verification

**Files:**
- Modify: `mods/PalworldEditor/src/dllmain.cpp`
- Modify: `README.md`
- Modify: `AGENTS.md`
- Modify: `CLAUDE.md`
- Add: `docs/superpowers/specs/2026-07-25-skill-catalog-crash-localization-design.md`
- Add: `docs/superpowers/plans/2026-07-25-skill-catalog-crash-localization.md`

**Interfaces:**
- Consumes: the completed independent and localized catalog behavior.
- Produces: consistent v1.4.4 metadata, operational documentation, and final build evidence.

- [ ] **Step 1: Update all user-visible and metadata versions**

In `dllmain.cpp`, change exactly:

```cpp
ModVersion = STR("1.4.4");
Output::send<LogLevel::Verbose>(STR("PalworldEditor loaded (v1.4.4)\n"));
if (ImGui::Begin("PalworldEditor v1.4.4", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
```

Change repository documentation references from `1.4.3` to `1.4.4` in `README.md`, `AGENTS.md`,
and `CLAUDE.md`.

- [ ] **Step 2: Document catalog safety and update procedure**

Add these facts to the architecture and verification sections of all three documents, using each
document's existing Chinese style:

- active skill values and Raw IDs come from the generated
  `inc/skills/active_skill_definitions.hpp`;
- display names are queried from `PalUIUtility` and follow the game's language;
- passive and active catalog failures are independent and fall back to Raw IDs/previous data;
- `scripts/generate-active-skill-definitions.ps1` must be rerun after updating the Palworld UHT
  dump for a new game version;
- game verification must cover refresh without a crash, both selectable pickers, localized names,
  active value mapping, and target-switch invalidation.

Update the repository tree in `README.md` to include:

```text
│   │   ├── active_skill_definitions.hpp  Palworld 1.0 主动技能数值/Raw ID 生成表
```

- [ ] **Step 3: Regenerate and format**

Run:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/generate-active-skill-definitions.ps1
cmake --build --preset ninja-msvc-x64 --target format
```

Expected: the active table regenerates successfully and C++ formatting completes.

- [ ] **Step 4: Run complete automated verification**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor PalworldEditorTests
ctest --test-dir build --output-on-failure
git diff --check
rg -n "1\\.4\\.3|GetEnumNames|StaticFindObject<UEnum|activeIds_" mods/PalworldEditor README.md AGENTS.md CLAUDE.md
```

Expected:

- all build targets succeed;
- CTest reports `100% tests passed`;
- `git diff --check` succeeds;
- the final `rg` command reports no matches.

- [ ] **Step 5: Review the complete diff against the design**

Run:

```powershell
git diff --stat HEAD~3
git diff HEAD~3 -- mods/PalworldEditor/inc/skills mods/PalworldEditor/src mods/PalworldEditor/tests scripts README.md AGENTS.md CLAUDE.md
```

Confirm:

- no `UEnum` storage API remains;
- generated entries contain no localized names;
- `PalPlayerInventoryData` is used only as a per-refresh localization context;
- each picker checks its own section's `ready`;
- localization errors do not make a non-empty section unavailable;
- no target-selection or skill-write semantics changed.

- [ ] **Step 6: Commit release metadata and documentation**

```powershell
git add mods/PalworldEditor/src/dllmain.cpp README.md AGENTS.md CLAUDE.md docs/superpowers/specs/2026-07-25-skill-catalog-crash-localization-design.md docs/superpowers/plans/2026-07-25-skill-catalog-crash-localization.md
git commit -m "docs: release PalworldEditor 1.4.4"
```

- [ ] **Step 7: Record the required game validation**

Do not claim runtime completion until a Chinese-language Palworld 1.0 session verifies:

1. “刷新技能列表” no longer crashes.
2. Passive add/replace picker opens, searches, selects, and submits.
3. Active equip/replace picker opens, searches, selects, and submits.
4. Both catalogs show current-language names with Raw IDs.
5. Equipped values such as `15` and `124` show mapped names rather than decimal-only labels.
6. Clearing an active slot and removing a passive skill still work.
7. Changing the highlighted party Pal invalidates the old target and stale requests.
8. Refreshing before localization context exists shows Raw IDs/a non-fatal message; refreshing later
   restores localized names.

If the agent cannot launch the user's game installation, hand off the built DLL path and this exact
checklist without claiming that game validation passed.
