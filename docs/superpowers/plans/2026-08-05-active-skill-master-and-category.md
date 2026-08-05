# 主动技能强制掌握 + Category 分类 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 修复「装未掌握的主动技能后游戏效果异常」（装前写 MasteredWaza 强制掌握），并给主动技能目录加 Category（近战/射击/辅助）分类过滤。

**Architecture:** `rewrite_active` 装 `AddEquipWaza` 前，对未掌握的技能反射写 `SaveParameter.MasteredWaza`（追加 EPalWazaID）+ OnRep 刷新 + 重读验证 + 失败回滚（沿用被动/工作适应性的安全域模式）。`load_catalog` 调 `PalUtility::GetWazaDatabase` + `PalWazaDatabase::FindWazaForBP` 读每个技能的 Category。UI 加 Category 下拉 + `std::ranges::views::filter`。

**Tech Stack:** C++23、UE4SS 反射、CMake/Ninja、imgui 1.92、std::ranges。

## Global Constraints

- **不改被动技能、属性、形态、资源共享**；不升版本号。
- 所有构建在 **VS x64 开发者环境**（PowerShell VsDevShell）。
- 验证命令（每任务构建步骤都跑）：
  ```powershell
  cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor PalworldEditorTests PalworldEditorBaseResourceSharingTests
  ctest --test-dir build --output-on-failure
  ```
- 项目无反射/UI 单测；验证 = 构建 + ctest（保证不破纯领域）+ 人工游戏内冒烟（反射/UI 改动）。
- 提交前 `git diff --check`；commit message 结尾 `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`。
- clang-format 失败时跑 `D:/scoop/apps/llvm/current/bin/clang-format.exe -style=file -i <files>` 后重验。

---

## Task 1: ActiveSkillCategory 枚举 + ActiveSkillOption.category

**Files:**
- Modify: `mods/PalworldEditor/inc/skills/skill_catalog.hpp`

**Interfaces:**
- Produces: `enum class ActiveSkillCategory : std::uint8_t { Melee=0, Shot=1, Support=2 }`；`ActiveSkillOption::category` 字段（`std::optional<ActiveSkillCategory>`）。

- [ ] **Step 1：加 ActiveSkillCategory 枚举 + ActiveSkillOption.category**

在 `skill_catalog.hpp` 的 `ActiveSkillOption` 结构体定义之前加枚举，结构体内加字段：

```cpp
/** @brief 主动技能的战斗类型，值与 Palworld EPalWazaCategory 对齐。 */
enum class ActiveSkillCategory : std::uint8_t {
    Melee = 0,   // 近战
    Shot = 1,    // 射击
    Support = 2, // 辅助
};
```

在 `ActiveSkillOption` 结构体内（`localizedName` 之后）加：

```cpp
    std::optional<ActiveSkillCategory> category; /**< 从 PalWazaDatabase 读到的战斗类型；未读到为空。 */
```

若 `skill_catalog.hpp` 顶部没有 `#include <optional>`，补上。

- [ ] **Step 2：format-check + 构建 + ctest**

跑 Global Constraints 的验证命令。Expected：全绿（纯字段添加，不破坏现有）。

- [ ] **Step 3：提交**

```bash
git add mods/PalworldEditor/inc/skills/skill_catalog.hpp
git commit -m "feat(skills): add ActiveSkillCategory enum + ActiveSkillOption.category field

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Task 2: load_catalog 读 Category（FindWazaForBP）

**Files:**
- Modify: `src/skills/pal_skills.cpp`（`load_catalog` 的 active 部分）

**Interfaces:**
- Consumes: `ActiveSkillCategory`、`ActiveSkillOption.category`（Task 1）；`pal_game::invoke<T>`、`FunctionParams`（common/game_reflection.hpp）。
- Produces: `load_catalog` 填每个 `ActiveSkillOption.category`。

- [ ] **Step 1：在 load_catalog 里调 GetWazaDatabase + FindWazaForBP 读 Category**

在 `load_catalog` 的 active skill 填充处（`make_active_skill_options` 调用前后），加 Category 读取。先拿 `UPalWazaDatabase*`，再对每个 skill 调 `FindWazaForBP`。

```cpp
// 在 load_catalog 内，拿到 utility/worldContext 后：
auto* const wazaDbFunction =
    find_function<UFunction>(STR("/Script/Pal.PalUtility:GetWazaDatabase"));
UObject* wazaDatabase{};
if (wazaDbFunction != nullptr) {
    struct WazaDbParams {
        UObject* WorldContextObject{};
        UObject* ReturnValue{};
    } wazaParams{.WorldContextObject = worldContext};
    utility->ProcessEvent(wazaDbFunction, &wazaParams);
    wazaDatabase = wazaParams.ReturnValue;
}

auto* const findWazaFunction = pal_game::is_valid(wazaDatabase)
    ? wazaDatabase->GetFunctionByNameInChain(STR("FindWazaForBP"))
    : nullptr;
```

然后在 `make_active_skill_options` 的 localizer lambda 里（或之后遍历 catalog.active.skills），对每个 skill 调 FindWazaForBP 读 Category。FindWazaForBP 的 OutData 是 `FPalWazaDatabaseRaw`（out-param struct，非平凡），用 `FunctionParams` 按 ParmsSize 初始化：

```cpp
// 对每个 definition（或每个 catalog.active.skills[i]）：
if (findWazaFunction != nullptr && pal_game::is_valid(wazaDatabase)) {
    pal_game::FunctionParams fp{findWazaFunction};
    // 设置入参 Type（EPalWazaID）
    auto* typeProp = findWazaFunction->FindProperty(FName(STR("Type"), FNAME_Find));
    // ... 按 typeProp 类型（FEnumProperty/FByteProperty）写入 definition.value
    wazaDatabase->ProcessEvent(findWazaFunction, fp.data());
    // 读 OutData.Category（EPalWazaCategory，FEnumProperty，在 OutData struct 内）
    auto* outDataProp = CastField<FStructProperty>(
        findWazaFunction->FindProperty(FName(STR("OutData"), FNAME_Find)));
    if (outDataProp != nullptr) {
        void* outDataPtr = outDataProp->ContainerPtrToValuePtr<void>(fp.data());
        auto* categoryProp = CastField<FEnumProperty>(
            outDataProp->GetStruct()->FindProperty(FName(STR("Category"), FNAME_Find)));
        if (categoryProp != nullptr) {
            const int catValue = categoryProp
                ->GetUnderlyingProperty()->GetSignedIntPropertyValue(
                    categoryProp->ContainerPtrToValuePtr<void>(outDataPtr));
            if (catValue >= 0 && catValue <= 2) {
                option.category = static_cast<ActiveSkillCategory>(catValue);
            }
        }
    }
}
```

> **实现注**：`Type` 入参的写入方式（FEnumProperty vs FByteProperty）和 `OutData` 的精确布局需构建时按编译错误调整。核心是 `FindWazaForBP` 的两个参数：`Type`（EPalWazaID in）和 `OutData`（FPalWazaDatabaseRaw out）。`FunctionParams` 按 `findWazaFunction->GetParmsSize()` 分配 + InitializeStruct，写 Type 后 ProcessEvent，再从 OutData 偏移读 Category。

- [ ] **Step 2：format-check + 构建 + ctest**

跑验证命令。Expected：全绿。若 FindWazaForBP 反射布局报编译错，按编译器提示调整入参/OutData 的属性查找（Type/OutData/Category 的属性名与偏移）。

- [ ] **Step 3：人工冒烟（可选，部署后）**

部署进游戏，选一个帕鲁，看 UE4SS 日志（加临时 Verbose 日志打印读到的 Category）或直接看 UI（Task 3 完成后）。

- [ ] **Step 4：提交**

```bash
git add mods/PalworldEditor/src/skills/pal_skills.cpp
git commit -m "feat(skills): read active skill Category from PalWazaDatabase

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Task 3: UI Category 下拉 + ranges filter

**Files:**
- Modify: `src/skills/skill_ui.cpp`（`render_active_skills`）

**Interfaces:**
- Consumes: `ActiveSkillOption.category`（Task 1/2）；`std::ranges::views::filter`。
- Produces: `render_active_skills` 的 Category 下拉过滤。

- [ ] **Step 1：render_active_skills 加 Category 下拉 + filter**

在 `render_active_skills` 函数开头（主动技能列表渲染之前），加 Category 下拉。用 `self->activeCategoryFilter_`（需在 `mod_core.hpp` 的 `PalworldEditorMod` 类加成员 `std::optional<ActiveSkillCategory> activeCategoryFilter_`，或用 static——按项目惯例放 self 成员更合理）。

先在 `mod_core.hpp` 的 `PalworldEditorMod` private 成员区加：

```cpp
    std::optional<skill_editor::ActiveSkillCategory> activeCategoryFilter_;
```

然后在 `render_active_skills`（skill_ui.cpp），在现有 active skill 列表渲染前加下拉 + filter：

```cpp
// Category 下拉
if (ImGui::BeginCombo("类别##active-category",
                      self->activeCategoryFilter_.has_value() ? "已选类别" : "全部")) {
    if (ImGui::Selectable("全部", !self->activeCategoryFilter_.has_value())) {
        self->activeCategoryFilter_.reset();
    }
    for (const auto cat : {skill_editor::ActiveSkillCategory::Melee,
                           skill_editor::ActiveSkillCategory::Shot,
                           skill_editor::ActiveSkillCategory::Support}) {
        const bool isCurrent = self->activeCategoryFilter_ == cat;
        if (ImGui::Selectable(cat == skill_editor::ActiveSkillCategory::Melee   ? "近战"
                              : cat == skill_editor::ActiveSkillCategory::Shot  ? "射击"
                                                                                : "辅助",
                              isCurrent)) {
            self->activeCategoryFilter_ = cat;
        }
    }
    ImGui::EndCombo();
}
```

然后过滤 `snapshot.catalog.active.skills`（排除 excludedIds 后，再按 Category 过滤），用 ranges：

```cpp
// 现有 excluded 逻辑后，加 Category 过滤
const auto categoryMatches = [&](const skill_editor::SkillOption& option) {
    return !self->activeCategoryFilter_ ||
           (option.category && *option.category == *self->activeCategoryFilter_);
};
```

在现有 `render_skill_picker` 调用前，把 Category filter 作为额外排除条件（或在 picker 的 visible 列表里加 category 判断）。具体：`render_active_skills` 现有用 `render_skill_picker` 渲染下拉，其 visible 由 `filter_skills` 控制。最简方案是在 `render_skill_picker` 之前，手动 filter 一份再传——但 `render_skill_picker` 接收的是完整 options + excludedIds + search buffer。

**最务实的改法**：在 `render_skill_picker` 的搜索回调里，把 Category 不匹配的也视为 excluded。即给 `render_skill_picker` 的 `excludedIds` 额外合并 Category 不匹配的 ID。或更直接：在 `render_active_skills` 里，先按 Category 过滤出一份 `visibleOptions`，再传给一个简化版 picker。

> **实现注**：`render_skill_picker` 签名不变（它只按 id/excluded/search 过滤）。Category 过滤的最干净方式：在 `render_active_skills` 里构造一个 `visibleOptions`（`std::vector<SkillOption>`），用 `std::ranges::views::filter` 过滤 Category 后拷贝，传给 `render_skill_picker`（替代原 `snapshot.catalog.active.skills`）。这样 picker 的搜索/excluded 仍在 visibleOptions 上工作。

```cpp
std::vector<skill_editor::SkillOption> visibleActiveSkills;
std::ranges::copy_if(snapshot.catalog.active.skills, std::back_inserter(visibleActiveSkills),
                     categoryMatches);
// 用 visibleActiveSkills 替代 snapshot.catalog.active.skills 传给 render_skill_picker
```

- [ ] **Step 2：format-check + 构建 + ctest**

跑验证命令。Expected：全绿。

- [ ] **Step 3：人工冒烟（部署后）**

选帕鲁 → 主动技能区 → Category 下拉切换（全部/近战/射击/辅助）→ 确认列表按类过滤。

- [ ] **Step 4：提交**

```bash
git add mods/PalworldEditor/inc/mod/mod_core.hpp mods/PalworldEditor/src/skills/skill_ui.cpp
git commit -m "feat(skills): add Category dropdown filter to active skill picker

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Task 4: rewrite_active 强制掌握（MasteredWaza 反射写）

**Files:**
- Modify: `src/skills/pal_skills.cpp`（`rewrite_active`）

**Interfaces:**
- Consumes: `pal_game::invoke<bool>`（HasMasteredWaza）；SaveParameter 反射导航模式（pal_stats/pal_identity）。
- Produces: `rewrite_active` 装技能前先确保 MasteredWaza。

- [ ] **Step 1：加 MasteredWaza 反射写 helper（匿名命名空间）**

在 `pal_skills.cpp` 匿名命名空间内，加 SaveParameter 导航 + MasteredWaza 反射写 helper：

```cpp
/** @brief 反射写 SaveParameter.MasteredWaza（追加 wazaID）+ OnRep 刷新；返回是否成功。 */
auto ensure_mastered(UObject* pal, const std::uint16_t wazaValue) -> bool {
    // 1. HasMasteredWaza 检查（UFunction）
    const auto mastered = pal_game::invoke<bool>(pal, STR("HasMasteredWaza"),
        [&]()-> std::uint16_t { return wazaValue; });  // 注：HasMasteredWaza(EPalWazaID) 有入参，需 FunctionParams
    // HasMasteredWaza 有入参 EPalWazaID，不能直接用 invoke<T>（invoke 只覆盖无参）
    // 改用 FunctionParams 手动调：
    ...
}
```

> **实现注**：`HasMasteredWaza(EPalWazaID)` 有入参，不能直接用 `pal_game::invoke<bool>`（invoke 只覆盖无参 getter）。需用 `FunctionParams` + 设入参 `EPalWazaID`（Type 属性）+ ProcessEvent + 读 ReturnValue（bool）。

实际 helper 实现（参考 pal_identity 的 `database_bool` 模式——有入参的 UFunction 调用）：

```cpp
[[nodiscard]] auto has_mastered_waza(UObject* pal, const std::uint16_t wazaValue) -> bool {
    auto* const fn = pal->GetFunctionByNameInChain(STR("HasMasteredWaza"));
    if (fn == nullptr) { return false; }
    pal_game::FunctionParams params{fn};
    // 设入参 EPalWazaID（参数名可能是 "WazaID"，按 FindProperty 查）
    auto* wazaProp = fn->FindProperty(FName(STR("WazaID"), FNAME_Find));
    // ... 按 wazaProp 类型写入 wazaValue（FEnumProperty/FByteProperty uint16）
    pal->ProcessEvent(fn, params.data());
    // 读 ReturnValue（bool）
    auto* retProp = CastField<FBoolProperty>(fn->FindProperty(FName(STR("ReturnValue"), FNAME_Find)));
    return retProp != nullptr ? retProp->GetPropertyValueInContainer(params.data()) : false;
}
```

MasteredWaza 反射写（追加）——参考 `pal_stats.cpp` 的 `write_work_suitability_bonuses_in_place`（FArrayProperty TArray 写）模式：

```cpp
auto append_mastered_waza(UObject* pal, const std::uint16_t wazaValue) -> bool {
    // 导航 SaveParameter
    auto* const saveProp = CastField<FStructProperty>(pal->GetPropertyByNameInChain(STR("SaveParameter")));
    if (saveProp == nullptr) { return false; }
    auto* const saveStruct = saveProp->GetStruct().Get();
    void* const saveParam = saveProp->ContainerPtrToValuePtr<void>(pal);
    // MasteredWaza FArrayProperty
    auto* const arrayProp = CastField<FArrayProperty>(saveStruct->FindProperty(FName(STR("MasteredWaza"), FNAME_Find)));
    if (arrayProp == nullptr) { return false; }
    // 追加：FScriptArrayHelper_InContainer，扩容 Num+1，写 wazaValue（uint16）到尾部
    FScriptArrayHelper_InContainer arr(arrayProp, saveParam);
    const int32 newIndex = arr.Num();
    arr.AddValue();
    // 写 EPalWazaID（uint16）到 newIndex
    auto* innerEnum = CastField<FEnumProperty>(arrayProp->GetInner());
    // ... 按 inner 类型写入
    // OnRep 刷新
    auto* const onRep = pal->GetFunctionByNameInChain(STR("OnRep_SaveParameter"));
    if (onRep != nullptr && onRep->GetParmsSize() == 0) {
        pal->ProcessEvent(onRep, nullptr);
    }
    return true;
}
```

- [ ] **Step 2：rewrite_active 集成强制掌握 + 验证 + 回滚**

修改 `rewrite_active`：

```cpp
auto PalSkillGateway::rewrite_active(const skill_editor::SkillTarget target,
                                     const std::span<const skill_editor::ActiveSkill> skills) -> bool {
    auto* pal = to_pal(target);
    // ... 现有预检（find clear/add functions, skills.size() <= 3）

    // 读 before（原 EquipWaza + 原 MasteredWaza），用于回滚
    // 原 EquipWaza：调 GetEquipWaza（已有 read_state 的逻辑，或单独读）
    // 原 MasteredWaza：调 GetMasteredWaza（UFunction，返回 TArray）

    // 对每个 target skill：确保掌握
    for (const auto& skill : skills) {
        if (!has_mastered_waza(pal, skill.value)) {
            if (!append_mastered_waza(pal, skill.value)) {
                return false;  // 掌握写入失败
            }
        }
    }

    // 现有流程：ClearEquipWaza + AddEquipWaza
    pal->ProcessEvent(clearFunction, nullptr);
    for (const auto& skill : skills) {
        // ... AddEquipWaza
    }

    // 验证：GetEquipWaza 前3 == target + 每个 target HasMasteredWaza
    // 失败回滚：恢复原 MasteredWaza + EquipWaza
    // （回滚逻辑参考 pal_identity apply_identity_edit 的 rollback 模式）

    return true;
}
```

> **实现注**：回滚需恢复原 MasteredWaza（反射写回 before 值）。最简：before 时调 `GetMasteredWaza`（UFunction 返回 TArray<EPalWazaID>）存一份，失败时反射写回。EquipWaza 回滚 = ClearEquipWaza + AddEquipWaza(before)。具体回滚的实现深度按构建/人工验证迭代。

- [ ] **Step 3：format-check + 构建 + ctest**

跑验证命令。Expected：全绿。

- [ ] **Step 4：人工冒烟（部署后，关键）**

选帕鲁 → 主动技能区 → 从目录选一个帕鲁**没掌握**的技能 → 装备 → 应用 → **召唤后该技能效果正常**（不再异常）。
- 验证 HasMasteredWaza 后该技能已掌握（可调 GetMasteredWaza 确认）。
- 验证已掌握的技能（重排）仍正常。
- 验证写入失败时回滚（不污染存档）。

- [ ] **Step 5：提交**

```bash
git add mods/PalworldEditor/src/skills/pal_skills.cpp
git commit -m "fix(skills): ensure MasteredWaza before AddEquipWaza

Active skills the pal hasn't mastered would equip but have broken
in-game effects (no skill data). Now rewrite_active writes
SaveParameter.MasteredWaza (append EPalWazaID) + OnRep before equipping,
with read-back verification and rollback on failure.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Task 5: 最终验证

**Files:** 无（仅验证）。

- [ ] **Step 1：全量构建 + format-check + ctest + git diff --check**

```powershell
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor PalworldEditorTests PalworldEditorBaseResourceSharingTests
ctest --test-dir build --output-on-failure
git diff --check
git status --short
```

Expected：三目标构建成功；ctest 2/2；diff --check 无输出；工作树干净。

- [ ] **Step 2：人工游戏内冒烟（对照 spec 验收）**

- 装未掌握的主动技能 → 效果正常（核心 bug 修复）。
- Category 下拉过滤（全部/近战/射击/辅助）正常。
- 装已掌握的（重排）正常。
- 写入失败回滚（不污染）。
