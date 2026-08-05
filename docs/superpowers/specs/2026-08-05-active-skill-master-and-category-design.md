# 主动技能强制掌握 + Category 分类 Design

**日期：** 2026-08-05
**目标：** 修复"装未掌握的主动技能后游戏效果异常"（B 方案：装前强制掌握）；给主动技能目录加 Category（近战/射击/辅助）分类过滤。不改被动技能、属性、形态、资源共享。

## 背景

### 装未掌握技能效果异常
- `PalSkillGateway::rewrite_active` 只调 `ClearEquipWaza` + `AddEquipWaza`（装槽），完全不碰 `MasteredWaza`。
- `active_skill_definitions` 是 `EPalWazaID` 全表，用户可挑帕鲁**没掌握**的技能装。
- `AddEquipWaza` 装了未掌握的 ID，但 `MasteredWaza` 里没有 → 游戏无该技能数据 → 发动效果异常。
- 游戏没暴露 `AddMasteredWaza` UFunction（只有 `HasMasteredWaza` / `GetMasteredWaza`），但 `MasteredWaza` 是存档字段 `PalIndividualCharacterSaveParameter.MasteredWaza`（`TArray<EPalWazaID>`），可反射写。

### Category 来源
- `PalUtility::GetWazaDatabase(WorldContextObject)` → `UPalWazaDatabase*`。
- `UPalWazaDatabase::FindWazaForBP(EPalWazaID Type, FPalWazaDatabaseRaw& OutData)` → bool；`FPalWazaDatabaseRaw` 含 `Category`（`EPalWazaCategory`：Melee/Shot/Support）+ `Element`（`EPalWazaElementType`）。
- `EPalWazaCategory.h` 在 dump（Melee/Shot/Support）。`EPalWazaElementType.h` **dump 缺失**（文件不存在），Element 本设计不做。

## A. 强制掌握 + 装（`rewrite_active`）

### 新流程
1. 读 before：原 `GetEquipWaza()`（`TArray<EPalWazaID>`）+ 原存档 `MasteredWaza`（反射读），用于回滚。
2. 对每个 target skill（要装的，≤3）：
   - 调 `HasMasteredWaza(skill.value)`（UFunction）。
   - 未掌握 → 反射写 `SaveParameter.MasteredWaza`（追加该 `EPalWazaID`）+ `OnRep_SaveParameter` 刷新。
3. `ClearEquipWaza` + `AddEquipWaza`（装 target，现有流程）。
4. 验证：`GetEquipWaza` 前 3 == target，且每个 target `HasMasteredWaza` == true。
5. 失败回滚：恢复原 `MasteredWaza`（反射写回 before）+ 原 `EquipWaza`（`ClearEquipWaza` + 逐个 `AddEquipWaza` before），重读验证。

### `MasteredWaza` 反射写
- 导航 `SaveParameter`：`pal->GetPropertyByNameInChain("SaveParameter")` → `FStructProperty` → `SaveStruct` + `saveParam`（同 pal_stats / pal_identity 模式）。
- `MasteredWaza`：`SaveStruct->FindProperty("MasteredWaza")` → `FArrayProperty`（inner = `EPalWazaID` 的 `FEnumProperty`/`FByteProperty`）。
- 追加：`FScriptArrayHelper_InContainer` 读原 `Num`，在尾部写入新 `EPalWazaID`（uint16）。
- 刷新：`pal->ProcessEvent(OnRep_SaveParameter, nullptr)`。

### 安全域
- 沿用被动/工作适应性的「反射写存档 + OnRep + 重读验证 + 失败回滚」模式。
- 回滚失败 → 整笔拒绝（不污染存档）。

## B. Category 分类（Melee/Shot/Support）

### 元数据
- mod 定义 `ActiveSkillCategory{Melee, Shot, Support}`（值对齐 `EPalWazaCategory` 0/1/2），放 `active_skill_definitions.hpp` 或 `skill_catalog.hpp`。
- `ActiveSkillOption` 加 `std::optional<ActiveSkillCategory> category`（load 时填，取不到 = nullopt）。

### `load_catalog` 读 Category
- 调 `PalUtility.GetWazaDatabase(worldCtx)`（UFunction）→ `UPalWazaDatabase*`。
- 对每个 active skill（`EPalWazaID`）：调 `PalWazaDatabase.FindWazaForBP(wazaID, outData)`（UFunction，OutData 是 `FPalWazaDatabaseRaw` out-param，非平凡 struct，需 `FunctionParams` 按 `ParmsSize` 初始化）→ 读 `outData.Category`（`EPalWazaCategory`，`FEnumProperty` 偏移）→ 转 `ActiveSkillCategory`。
- 存 `ActiveSkillOption.category`。

### UI（`render_active_skills`）
- Category 下拉（全量 / 近战 / 射击 / 辅助），放在主动技能列表上方（类似被动 `render_passive_category_picker`）。
- `std::optional<ActiveSkillCategory> catFilter`（nullopt = 全量，即 `*`）。
- 过滤：`std::ranges::views::filter([&](const auto& s) { return !catFilter || (s.category && *s.category == *catFilter); })`（category 为 nullopt 的 skill 在选定具体类时被滤掉）。

## C. Element —— 本次不做

`EPalWazaElementType.h` dump 缺失，无法准确知道属性枚举值。等 UHT dump 补全后再加 Element 维度（双维 AND）。

## 验证

- 构建：`format-check` + `PalworldEditor` + `PalworldEditorTests` + `PalworldEditorBaseResourceSharingTests` + `ctest` 全绿。
- 游戏内冒烟（人工）：
  - 装一个帕鲁**未掌握**的主动技能 → 应用后技能效果正常（不再异常）。
  - Category 下拉过滤（全量 / Melee / Shot / Support）正确。
  - 装已掌握的技能（重排）仍正常。
  - 写入失败时回滚（存档不污染）。

## 非目标

- Element 分类（dump 缺 `EPalWazaElementType`）。
- 主动 Category 的后台增量任务（主动技能数量有限，在 `load_catalog` 一次性读，不像被动分类需分批增量）。
- 主动技能目录的其它改动（数量上限 3、排序、Raw ID 兜底等不变）。
