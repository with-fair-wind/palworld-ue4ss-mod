# 当前待出战帕鲁技能目标修复 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让 PalworldEditor 始终编辑玩家队伍中通过 Q/E 选中的、下一次会被投出战斗的帕鲁，并移除点击扫描结果导致崩溃的旧目标选择路径。

**Architecture:** 游戏线程每帧从稳定的 `PalPlayerInventoryData` 世界上下文开始，经 `PalUtility`、`PalOtomoHolderComponentBase` 和 `PalIndividualCharacterHandle` 解析当前选中帕鲁参数对象。纯 C++ 状态对象负责目标切换检测与过期编辑请求拦截；UE4SS 反射适配层只负责把 Unreal 对象转换为该状态。技能目录始终使用稳定世界上下文加载，不再把帕鲁参数对象当作 `WorldContextObject`。

**Tech Stack:** C++23、UE4SS、Unreal 反射、CMake/Ninja、CTest。

## Global Constraints

- 所有 Unreal 对象访问和 `ProcessEvent` 调用只在游戏线程执行。
- 不跨帧保存扫描所得的原始 `UObject*`；仅保存当前帧解析出的目标标识和面向 GUI 的值快照。
- 编辑请求必须携带入队时的目标，执行前再次与当前选中目标比对；目标已切换时拒绝执行。
- 所有新增或修改的类、接口和字段继续使用中文 Doxygen 注释。
- 不修改 `RE-UE4SS/` 第三方代码，不提交用户文件 `UHTHeaderDump.7z`。
- 本仓库没有可脱离游戏运行的 Unreal 集成测试；纯状态与队列行为使用 CTest 覆盖，反射边界通过 UHT 声明核对、完整构建和游戏内验收验证。
- 按用户此前授权跳过耗时且长期无输出的完整 `tidy-check`；仍执行格式检查、编译、CTest 和差异检查。

---

## Task 1：为当前选中目标建立纯 C++ 状态与过期请求保护

**Files:**

- Create: `mods/PalworldEditor/inc/skills/selected_target_state.hpp`
- Modify: `mods/PalworldEditor/tests/skill_editor_tests.cpp`

- [ ] **Step 1：先编写失败测试**

在测试文件中引入新头文件：

```cpp
#include <skills/selected_target_state.hpp>
```

增加以下测试，并在 `main()` 中显式调用：

```cpp
void test_selected_target_state_tracks_identity_changes()
{
    skill_editor::SelectedTargetState state;

    CHECK(!state.update({}));
    CHECK(state.update({.target = 0x1000, .name = "Boar"}));
    CHECK(state.current().target == 0x1000);
    CHECK(state.current().name == "Boar");

    CHECK(!state.update({.target = 0x1000, .name = "Wild Boar"}));
    CHECK(state.current().name == "Wild Boar");

    CHECK(state.update({.target = 0x2000, .name = "Chicken"}));
    CHECK(state.update({}));
    CHECK(state.current().target == 0);
}

void test_edit_request_must_still_target_current_selection()
{
    const skill_editor::SelectedTargetObservation current{
        .target = 0x2000,
        .name = "Chicken",
    };
    const skill_editor::SkillEditRequest currentRequest{
        .target = 0x2000,
    };
    const skill_editor::SkillEditRequest staleRequest{
        .target = 0x1000,
    };

    CHECK(skill_editor::request_targets_current(currentRequest, current));
    CHECK(!skill_editor::request_targets_current(staleRequest, current));
    CHECK(!skill_editor::request_targets_current({}, current));
    CHECK(!skill_editor::request_targets_current(currentRequest, {}));
}
```

- [ ] **Step 2：运行测试，确认因新头文件缺失而失败**

```powershell
$vcvarsPath = 'C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Auxiliary\Build\vcvars64.bat'
$cmdLine = 'call "{0}" >nul && cmake --build --preset ninja-msvc-x64 --target PalworldEditorTests' -f $vcvarsPath
& cmd.exe /d /s /c $cmdLine
```

预期：编译器报告找不到 `skills/selected_target_state.hpp`，证明测试处于 RED 状态。

- [ ] **Step 3：实现最小状态对象**

创建头文件，包含完整中文 Doxygen 注释：

```cpp
#pragma once

#include <skills/skill_editor_service.hpp>

#include <string>
#include <utility>

namespace skill_editor
{

/**
 * @brief 当前待出战帕鲁的一次观测结果。
 */
struct SelectedTargetObservation
{
    /** @brief 当前帕鲁参数对象对应的不透明目标句柄；0 表示没有可用目标。 */
    SkillTarget target{};

    /** @brief 用于界面展示的角色 ID 或名称。 */
    std::string name;
};

/**
 * @brief 保存当前待出战帕鲁，并判断目标身份是否发生变化。
 */
class SelectedTargetState
{
public:
    /**
     * @brief 用最新观测替换当前状态。
     * @param observation 当前帧观测结果。
     * @return 目标句柄发生变化时返回 true；仅名称变化时返回 false。
     */
    [[nodiscard]] auto update(SelectedTargetObservation observation) -> bool
    {
        const bool targetChanged = current_.target != observation.target;
        current_ = std::move(observation);
        return targetChanged;
    }

    /**
     * @brief 获取最近一次观测结果。
     * @return 当前观测结果的只读引用。
     */
    [[nodiscard]] auto current() const -> const SelectedTargetObservation&
    {
        return current_;
    }

private:
    /** @brief 最近一次写入的目标观测结果。 */
    SelectedTargetObservation current_;
};

/**
 * @brief 判断排队的编辑请求是否仍指向当前待出战帕鲁。
 * @param request 待执行编辑请求。
 * @param current 当前选中目标。
 * @return 两者目标均有效且句柄相同时返回 true。
 */
[[nodiscard]] inline auto request_targets_current(
    const SkillEditRequest& request,
    const SelectedTargetObservation& current) -> bool
{
    return current.target != 0 && request.target == current.target;
}

} // namespace skill_editor
```

- [ ] **Step 4：重新运行测试，确认通过**

```powershell
$cmdLine = 'call "{0}" >nul && cmake --build --preset ninja-msvc-x64 --target PalworldEditorTests && ctest --test-dir build -R PalworldEditor.SkillEditor --output-on-failure' -f $vcvarsPath
& cmd.exe /d /s /c $cmdLine
```

预期：`PalworldEditorTests` 构建成功，CTest 报告 `100% tests passed`。

- [ ] **Step 5：提交纯状态层**

```powershell
git add mods/PalworldEditor/inc/skills/selected_target_state.hpp mods/PalworldEditor/tests/skill_editor_tests.cpp
git commit -m "feat: track selected Pal target state"
```

---

## Task 2：通过稳定世界上下文解析当前待出战帕鲁

**Files:**

- Modify: `mods/PalworldEditor/inc/game/pal_game.hpp`
- Modify: `mods/PalworldEditor/inc/skills/pal_skills.hpp`
- Modify: `mods/PalworldEditor/src/pal_skills.cpp`
- Modify: `mods/PalworldEditor/src/dllmain.cpp`

- [ ] **Step 1：核对运行时反射契约**

运行：

```powershell
rg -n "GetOtomoHolderComponent|GetSelectedOtomoID|GetOtomoIndividualHandle|TryGetIndividualParameter|GetCharacterID" UHTHeaderDump/Pal/Public
```

预期确认：

- `PalUtility::GetOtomoHolderComponent(const UObject*)`
- `PalOtomoHolderComponentBase::GetSelectedOtomoID() -> int32`
- `PalOtomoHolderComponentBase::GetOtomoIndividualHandle(int32)`
- `PalIndividualCharacterHandle::TryGetIndividualParameter()`
- `PalIndividualCharacterParameter::GetCharacterID() -> FName`

- [ ] **Step 2：在游戏适配层增加稳定世界上下文和目标结果**

在 `pal_game.hpp` 中增加：

```cpp
/**
 * @brief 当前待出战帕鲁的运行时解析结果。
 */
struct SelectedPalTarget
{
    /** @brief 当前帕鲁的个体参数对象；解析失败时为空。 */
    RC::Unreal::UObject* parameter{};

    /** @brief 帕鲁 CharacterID 的 UTF-8 表示。 */
    std::string characterId;
};

/**
 * @brief 获取可安全作为 Pal 蓝图工具函数世界上下文的玩家背包对象。
 * @return 找到时返回 `PalPlayerInventoryData`；否则返回 nullptr。
 */
[[nodiscard]] inline auto get_world_context() -> RC::Unreal::UObject*
{
    return RC::Unreal::UObjectGlobals::FindFirstOf(kInventoryClassName);
}

/**
 * @brief 解析 Q/E 当前选中的下一只待出战帕鲁。
 * @return 参数对象与 CharacterID；任一步骤失败时返回空结果。
 */
[[nodiscard]] inline auto resolve_selected_otomo() -> SelectedPalTarget;
```

实现 `resolve_selected_otomo()` 时，为每个反射调用定义与 UHT 参数顺序一致的局部 `Params`，并逐步执行：

1. `get_world_context()`；
2. `/Script/Pal.PalUtility:GetOtomoHolderComponent`；
3. holder 的 `/Script/Pal.PalOtomoHolderComponentBase:GetSelectedOtomoID`；
4. holder 的 `/Script/Pal.PalOtomoHolderComponentBase:GetOtomoIndividualHandle`；
5. handle 的 `/Script/Pal.PalIndividualCharacterHandle:TryGetIndividualParameter`；
6. parameter 的 `/Script/Pal.PalIndividualCharacterParameter:GetCharacterID`。

每一步在调用前检查对象和 `UFunction*`，返回对象后立即检查；选中槽位小于 0 时直接返回空结果。对最终参数对象，用
`GetClassPrivate()->IsChildOf()` 验证其派生自 `/Script/Pal.PalIndividualCharacterParameter` 后再读取
`CharacterID`。不得把 holder、handle 或扫描列表中的对象缓存到下一帧。

- [ ] **Step 3：让技能目录脱离目标对象**

把：

```cpp
[[nodiscard]] auto load_catalog(skill_editor::SkillTarget contextTarget)
    -> skill_editor::SkillCatalogSnapshot;
```

改为：

```cpp
/**
 * @brief 从游戏运行时加载可分配的主动与被动技能目录。
 *
 * 本方法自行获取稳定的玩家背包世界上下文，不依赖当前帕鲁参数对象。
 */
[[nodiscard]] auto load_catalog() -> skill_editor::SkillCatalogSnapshot;
```

在 `pal_skills.cpp` 中删除 `contextTarget` 到 `UObject*` 的转换，改用：

```cpp
auto* const worldContext = pal_game::get_world_context();
if (worldContext == nullptr)
{
    return {.error = "PalPlayerInventoryData world context is unavailable"};
}
```

后续 `GetPalAssignablePassiveIDs`、`GetPassiveSkillName`、`GetWazaName` 均使用这个 `worldContext`。本任务暂时将
`dllmain.cpp` 中现有调用同步改为无参 `load_catalog()`，保持工程可编译；目标选择整体替换在下一任务完成。

- [ ] **Step 4：构建完整 DLL，验证反射参数和接口变更**

```powershell
$cmdLine = 'call "{0}" >nul && cmake --build --preset ninja-msvc-x64 --target PalworldEditor' -f $vcvarsPath
& cmd.exe /d /s /c $cmdLine
```

预期：`PalworldEditor` 构建成功，没有 `Params` 布局、`FName` 转换或旧 `load_catalog(target)` 调用错误。

- [ ] **Step 5：提交安全解析与目录上下文修复**

```powershell
git add mods/PalworldEditor/inc/game/pal_game.hpp mods/PalworldEditor/inc/skills/pal_skills.hpp mods/PalworldEditor/src/pal_skills.cpp mods/PalworldEditor/src/dllmain.cpp
git commit -m "fix: resolve selected Pal from stable world context"
```

---

## Task 3：接入每帧目标跟踪并删除危险扫描/界面钩子路径

**Files:**

- Modify: `mods/PalworldEditor/src/dllmain.cpp`
- Modify: `mods/PalworldEditor/inc/game/pal_game.hpp`
- Modify: `mods/PalworldEditor/tests/skill_editor_tests.cpp`

- [ ] **Step 1：补充“过期请求不调用网关”的失败测试**

在已有假网关中记录 `apply()` 调用次数，并增加一个纯辅助执行器测试。辅助接口放入
`selected_target_state.hpp`，只负责在进入真实网关前校验目标：

```cpp
template <typename Apply>
[[nodiscard]] auto apply_if_target_is_current(
    const SkillEditRequest& request,
    const SelectedTargetObservation& current,
    Apply&& apply) -> std::optional<SkillEditResult>;
```

测试要求：

- 请求目标与当前目标相同时，回调调用一次并返回结果；
- 请求目标过期、为 0 或当前无目标时，回调不被调用并返回 `std::nullopt`。

先只写测试并运行：

```powershell
$cmdLine = 'call "{0}" >nul && cmake --build --preset ninja-msvc-x64 --target PalworldEditorTests' -f $vcvarsPath
& cmd.exe /d /s /c $cmdLine
```

预期：因 `apply_if_target_is_current` 尚未定义而失败。

- [ ] **Step 2：实现最小过期请求执行器**

在 `selected_target_state.hpp` 中增加 `<functional>`、`<optional>`，实现：

```cpp
template <typename Apply>
[[nodiscard]] auto apply_if_target_is_current(
    const SkillEditRequest& request,
    const SelectedTargetObservation& current,
    Apply&& apply) -> std::optional<SkillEditResult>
{
    if (!request_targets_current(request, current))
    {
        return std::nullopt;
    }

    return std::invoke(std::forward<Apply>(apply), request);
}
```

补齐中文 Doxygen 后重新运行 `PalworldEditorTests`，预期通过。

- [ ] **Step 3：移除依赖详情界面的旧目标跟踪**

在 `dllmain.cpp` 中删除：

- 对 `GetPassiveSkillList` 的 `ProcessEvent` hook 注册；
- `lastViewedPal_`、`fnGetPSL_`、`suppressViewTracking_`；
- `ViewTrackingGuard`；
- 根据 `lastViewedPal_` 或手工点击项选择目标的 `resolve_skill_target()` 旧实现；
- “viewed/manual” 来源枚举和快照字段。

`on_unreal_init()` 只保留目录/函数初始化和已有安全初始化逻辑，不再依赖玩家打开帕鲁详情页。

- [ ] **Step 4：删除跨帧扫描对象与点击选择界面**

在 `dllmain.cpp` 中删除：

- `pal_cache_`；
- `pal_selected_`；
- `want_scan_pals_`；
- `Scan Pals` 按钮及列表；
- 所有点击扫描结果后将裸指针写入成员的代码。

若 `pal_game::PalEntry` 和 `pal_game::scan_pals()` 已无引用，从 `pal_game.hpp` 一并删除。保留与物品扫描有关的逻辑，
因为物品目录和背包显示不依赖这些帕鲁对象指针。

- [ ] **Step 5：在每个游戏帧解析并发布当前待出战目标**

在 mod 类中加入：

```cpp
/** @brief 当前 Q/E 选中的下一只待出战帕鲁状态。 */
skill_editor::SelectedTargetState selectedTarget_;
```

在 `on_update()` 游戏线程部分最先调用 `pal_game::resolve_selected_otomo()`，转为：

```cpp
const auto resolved = pal_game::resolve_selected_otomo();
const skill_editor::SelectedTargetObservation observation{
    .target = reinterpret_cast<skill_editor::SkillTarget>(resolved.parameter),
    .name = resolved.characterId,
};
const bool targetChanged = selectedTarget_.update(observation);
```

随后遵循以下顺序：

1. 解析当前目标；
2. 若目标变化，清空旧技能快照和旧错误；
3. 当前目标有效时读取主动/被动技能；
4. 处理目录首次加载或用户刷新；
5. 弹出一个编辑请求；
6. 用 `apply_if_target_is_current()` 再次校验；
7. 目标过期时生成“目标已切换，请重新提交”的 GUI 结果，不调用 `PalSkillGateway`；
8. 编辑成功后立即从同一当前目标重新读取技能；
9. 发布只含字符串、技能 ID 和状态码的 GUI 快照。

当目标为空时，GUI 快照必须清空，不能继续显示或编辑上一只帕鲁。

- [ ] **Step 6：更新 GUI 文案和启用条件**

编辑器标题区域显示：

```text
当前待出战帕鲁：<CharacterID>
```

无目标时显示：

```text
尚未解析到待出战帕鲁；请确认队伍中有帕鲁，并使用 Q/E 选择。
```

新增、修改、删除主动/被动技能按钮仅在当前目标有效且技能目录可用时启用。用户切换 Q/E 后，下一帧快照应更新，
无需打开帕鲁详情页或点击 Scan Pals。

- [ ] **Step 7：运行测试和完整构建**

```powershell
$cmdLine = 'call "{0}" >nul && cmake --build --preset ninja-msvc-x64 --target PalworldEditorTests && ctest --test-dir build -R PalworldEditor.SkillEditor --output-on-failure && cmake --build --preset ninja-msvc-x64 --target PalworldEditor' -f $vcvarsPath
& cmd.exe /d /s /c $cmdLine
```

预期：纯测试全部通过，DLL 构建成功。

- [ ] **Step 8：提交目标选择主流程**

```powershell
git add mods/PalworldEditor/inc/game/pal_game.hpp mods/PalworldEditor/inc/skills/selected_target_state.hpp mods/PalworldEditor/src/dllmain.cpp mods/PalworldEditor/tests/skill_editor_tests.cpp
git commit -m "fix: edit the currently selected combat Pal"
```

---

## Task 4：同步文档、格式化并完成可复现验证

**Files:**

- Modify: `README.md`
- Modify: `AGENTS.md`
- Modify: `CLAUDE.md`
- Modify: `mods/PalworldEditor/src/dllmain.cpp`
- Modify as needed: 本计划涉及的 C++ 文件

- [ ] **Step 1：更新版本与用户文档**

将 mod 版本从 `1.4.0` 更新为 `1.4.1`。README 中明确：

- 编辑目标是 Q/E 选中的下一只待出战帕鲁；
- 不需要打开帕鲁详情页；
- 已删除 `Scan Pals` 手工选择入口；
- 切换目标后旧编辑请求会被拒绝；
- 技能目录使用玩家背包对象作为稳定世界上下文。

同步更新 `AGENTS.md` 与 `CLAUDE.md` 中已经不符合实现的目标选择说明，确保两份指导文档语义一致。

- [ ] **Step 2：运行格式化和格式检查**

```powershell
$vcvarsPath = 'C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Auxiliary\Build\vcvars64.bat'
$cmdLine = 'call "{0}" >nul && cmake --build --preset ninja-msvc-x64 --target format && cmake --build --preset ninja-msvc-x64 --target format-check' -f $vcvarsPath
& cmd.exe /d /s /c $cmdLine
```

预期：格式化完成，`format-check` 成功。检查格式化差异，确保没有改动任务范围外的第三方文件。

- [ ] **Step 3：执行最终自动化验证**

```powershell
$cmdLine = 'call "{0}" >nul && cmake --build --preset ninja-msvc-x64 --target PalworldEditorTests && ctest --test-dir build -R PalworldEditor.SkillEditor --output-on-failure && cmake --build --preset ninja-msvc-x64 --target PalworldEditor' -f $vcvarsPath
& cmd.exe /d /s /c $cmdLine
git diff --check
git status --short
```

预期：

- `PalworldEditorTests` 为 `100% tests passed`；
- `PalworldEditor.dll` 构建成功；
- `git diff --check` 无输出；
- `git status --short` 只出现本任务预期文件及用户已有的 `UHTHeaderDump.7z`。

- [ ] **Step 4：部署并执行游戏内验收**

在用户已配置 `PALWORLD_INSTALL_DIR` 的 VS x64 环境中：

```powershell
$cmdLine = 'call "{0}" >nul && cmake --build --preset ninja-msvc-x64 --target deploy' -f $vcvarsPath
& cmd.exe /d /s /c $cmdLine
```

游戏内逐项检查：

1. 队伍中放入至少两只帕鲁；
2. 不打开帕鲁详情页，打开 PalworldEditor；
3. 用 Q/E 切换待出战帕鲁，确认编辑器目标在一帧或很短时间内同步变化；
4. 分别新增、修改、删除一项被动技能；
5. 分别新增、修改、删除一项主动技能；
6. 切换 Q/E 后立即提交旧界面请求，确认请求被安全拒绝且游戏不崩溃；
7. 打开帕鲁终端、状态页和背包，确认不再存在 Scan Pals 点击崩溃；
8. 收回/投出帕鲁后重新检查技能，确认修改作用于正确个体。

若游戏仍崩溃，保存最新 `.dmp` 与 UE4SS 日志，并记录崩溃前最后一次目标解析步骤；不要通过重新引入扫描裸指针绕过。

- [ ] **Step 5：提交文档和最终整理**

```powershell
git add README.md AGENTS.md CLAUDE.md mods/PalworldEditor
git commit -m "docs: explain selected combat Pal editing"
git status --short
```

预期：除用户已有的 `UHTHeaderDump.7z` 外工作区干净。
