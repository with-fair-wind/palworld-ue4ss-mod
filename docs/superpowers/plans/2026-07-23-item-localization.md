# Localized Item Labels Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Show runtime-localized item names as `名称 [RawId]` in the item browser and inventory while preserving raw IDs for every game operation.

**Architecture:** A new header-only `items/item_catalog.hpp` owns testable label, search, deduplication, sorting, and lookup logic. The game-thread item scan calls `PalUIUtility:GetItemName` once per loaded item definition and returns an immutable catalog snapshot; the ImGui thread reads only cached strings under the existing mutex.

**Tech Stack:** C++23, UE4SS reflection, Unreal `FText`, ImGui, CMake, Ninja, CTest, clang-format, clang-tidy

## Global Constraints

- Display format is `中文名 [RawId]`; inventory rows append ` ×数量`.
- Names follow the current Palworld language; Chinese game language produces Chinese names.
- Browser search matches both localized name and raw ID.
- Clicking a browser item writes only its raw ID into the Give input.
- Giving items and modifying inventory continue to use raw IDs and slot indexes.
- Missing localization dependencies or empty text fall back to raw ID without dropping the item.
- Unreal reflection runs only on the game thread.
- Do not unpack localization resources or restore a static item database.
- Do not modify `RE-UE4SS/`, `.gitignore`, `AGENTS.md`, or `UHTHeaderDump.7z`.

---

### Task 1: Add the pure localized item catalog

**Files:**
- Create: `mods/PalworldEditor/inc/items/item_catalog.hpp`
- Modify: `mods/PalworldEditor/tests/skill_editor_tests.cpp`

**Interfaces:**
- Produces: `item_catalog::ItemOption`, `item_catalog::ItemCatalogSnapshot`,
  `item_catalog::item_label()`, `item_catalog::matches_item()`,
  `item_catalog::filter_items()`, and `item_catalog::make_item_catalog()`.
- Consumes: only the C++ standard library; no UE4SS dependency.

- [ ] **Step 1: Write failing catalog tests**

Add `#include <items/item_catalog.hpp>` and tests equivalent to:

```cpp
void test_item_catalog_labels_and_search() {
    const item_catalog::ItemOption localized{.id = "PalSphere", .localizedName = "帕鲁球"};
    const item_catalog::ItemOption fallback{.id = "UnknownItem"};

    CHECK(item_catalog::item_label(localized) == "帕鲁球 [PalSphere]");
    CHECK(item_catalog::item_label(fallback) == "UnknownItem");
    CHECK(item_catalog::matches_item(localized, "帕鲁"));
    CHECK(item_catalog::matches_item(localized, "palsphere"));
    CHECK(!item_catalog::matches_item(localized, "木材"));
}

void test_item_catalog_deduplicates_indexes_and_sorts() {
    auto catalog = item_catalog::make_item_catalog({
        {.id = "Wood", .localizedName = "Zulu"},
        {.id = "PalSphere"},
        {.id = "PalSphere", .localizedName = "Alpha"},
        {.id = "Wood", .localizedName = "Repeated"},
    });

    CHECK(catalog.items.size() == 2);
    CHECK(item_catalog::item_label(catalog, "PalSphere") == "Alpha [PalSphere]");
    CHECK(item_catalog::item_label(catalog, "Missing") == "Missing");
    CHECK(catalog.items[0].id == "PalSphere");

    const auto filtered = item_catalog::filter_items(catalog, "alpha");
    CHECK(filtered.size() == 1);
    CHECK(filtered[0]->id == "PalSphere");
}
```

Call both tests from `main()`.

- [ ] **Step 2: Run the test target and verify RED**

Run in the VS x64 developer environment:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorTests
```

Expected: compilation fails because `items/item_catalog.hpp` does not exist.

- [ ] **Step 3: Implement the minimal header-only catalog**

Define:

```cpp
namespace item_catalog {

struct ItemOption {
    std::string id;
    std::string localizedName;
};

struct ItemCatalogSnapshot {
    std::vector<ItemOption> items;
    std::unordered_map<std::string, std::string> labelsById;
};

[[nodiscard]] auto item_label(const ItemOption& item) -> std::string;
[[nodiscard]] auto item_label(const ItemCatalogSnapshot& catalog, std::string_view id)
    -> std::string;
[[nodiscard]] auto matches_item(const ItemOption& item, std::string_view query) -> bool;
[[nodiscard]] auto filter_items(const ItemCatalogSnapshot& catalog, std::string_view query)
    -> std::vector<const ItemOption*>;
[[nodiscard]] auto make_item_catalog(std::vector<ItemOption> items) -> ItemCatalogSnapshot;

}
```

`make_item_catalog()` skips empty IDs, keeps the first non-empty localized name for each ID, sorts by final label, and builds
`labelsById`. ASCII folding must leave non-ASCII UTF-8 bytes unchanged so Chinese substring search remains valid.

- [ ] **Step 4: Run tests and verify GREEN**

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorTests
ctest --test-dir build --output-on-failure
```

Expected: `PalworldEditor.SkillEditor` passes.

- [ ] **Step 5: Commit**

```powershell
git add mods/PalworldEditor/inc/items/item_catalog.hpp mods/PalworldEditor/tests/skill_editor_tests.cpp
git commit -m "feat: add localized item catalog"
```

### Task 2: Load item names on the game thread and use them in both UIs

**Files:**
- Modify: `mods/PalworldEditor/inc/game/pal_game.hpp`
- Modify: `mods/PalworldEditor/src/dllmain.cpp`

**Interfaces:**
- Consumes: `item_catalog::make_item_catalog(std::vector<ItemOption>)`.
- Produces: `pal_game::scan_all_items() -> item_catalog::ItemCatalogSnapshot`.
- Uses: `/Script/Pal.Default__PalUIUtility`,
  `/Script/Pal.PalUIUtility:GetItemName`, and `PalPlayerInventoryData` as world context.

- [ ] **Step 1: Run failing source assertions**

```powershell
$source = Get-Content mods/PalworldEditor/inc/game/pal_game.hpp -Raw
if ($source -notmatch 'PalUIUtility:GetItemName') { exit 1 }
```

Expected: exit code 1 because runtime item localization is not implemented.

- [ ] **Step 2: Add the game-thread localization helper**

Include `<Unreal/FText.hpp>` and `<items/item_catalog.hpp>`. Add a helper that resolves the utility CDO with a
`FindFirstOf("PalUIUtility")` fallback and calls:

```cpp
struct Params {
    UObject* WorldContextObject;
    FName StaticItemId;
    FText OutName;
} params{.WorldContextObject = worldContext, .StaticItemId = id};
```

Return `text_encoding::to_utf8(params.OutName.ToString())`; return an empty string when utility, function, or world context is
unavailable.

- [ ] **Step 3: Return a localized catalog from the scanner**

Change `scan_all_items()` to collect `std::vector<item_catalog::ItemOption>`. For each scanned ID, attach the localized name and
return `item_catalog::make_item_catalog(std::move(items))`. Log the final unique item count.

- [ ] **Step 4: Integrate the catalog with browser and inventory rendering**

Change `item_db_cache_` to `item_catalog::ItemCatalogSnapshot`.

Browser requirements:

```cpp
const auto visible = item_catalog::filter_items(self->item_db_cache_, self->search_buf_);
for (const auto* item : visible) {
    const auto label = item_catalog::item_label(*item);
    if (ImGui::Selectable(label.c_str())) {
        // Copy item->id, never label, into item_buf_.
    }
}
```

Inventory requirements:

```cpp
const auto itemLabel = item_catalog::item_label(self->item_db_cache_, entry.item_id);
const auto label = itemLabel + "  x" + std::to_string(entry.count) + hiddenId;
```

Use the same localized label in the selected-item details while retaining the raw slot index and count.

- [ ] **Step 5: Verify source assertions and compile**

```powershell
rg -n "PalUIUtility:GetItemName|item_catalog::item_label|item_catalog::filter_items" mods/PalworldEditor
cmake --build --preset ninja-msvc-x64 --target PalworldEditor PalworldEditorTests
ctest --test-dir build --output-on-failure
```

Expected: source matches exist, both targets build, and `1/1` test passes.

- [ ] **Step 6: Commit**

```powershell
git add mods/PalworldEditor/inc/game/pal_game.hpp mods/PalworldEditor/src/dllmain.cpp
git commit -m "feat: show localized item labels"
```

### Task 3: Document and fully verify item localization

**Files:**
- Modify: `README.md`
- Verify: all task-owned files

**Interfaces:**
- Consumes: localized browser and inventory behavior.
- Produces: current user documentation and final verification evidence.

- [ ] **Step 1: Update README**

Document the `名称 [RawId]` format, dual-language search, runtime language behavior, raw-ID Give semantics, and ID fallback.
Add `inc/items/item_catalog.hpp` to the directory tree.

- [ ] **Step 2: Run formatting, build, and tests**

```powershell
cmake --build --preset ninja-msvc-x64 --target format
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor PalworldEditorTests
ctest --test-dir build --output-on-failure
```

Expected: formatting check and builds exit 0; `1/1` test passes.

- [ ] **Step 3: Run static analysis**

```powershell
cmake --build --preset ninja-msvc-x64 --target tidy-check
```

Expected: exit code 0 with configured non-fatal diagnostics.

- [ ] **Step 4: Verify raw-ID operations and workspace boundaries**

```powershell
rg -n "give_item_ = .*\\.id|modify_slot_ = e\\.slot_index" mods/PalworldEditor/src/dllmain.cpp
git diff --check
git diff --name-only -- RE-UE4SS
git status --short
```

Expected: browser selection copies an item ID, inventory modification uses the slot index, no whitespace or vendored changes
exist, and only known user-owned files remain outside task changes.

- [ ] **Step 5: Commit documentation**

```powershell
git add README.md
git commit -m "docs: describe localized item labels"
```
