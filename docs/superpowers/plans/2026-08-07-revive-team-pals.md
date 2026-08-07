# 复活队伍帕鲁 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 帕鲁 Tab 加「复活队伍帕鲁」按钮，一键复活所有死亡队伍帕鲁。

**Architecture:** GUI 线程按钮设原子请求 → game_thread_tick 消费 → 遍历 OtomoHolder 槽位 → SetPhysicalHealth(Healthful) 复活死亡帕鲁 → 结果消息发布。

**Tech Stack:** C++23、UE4SS 反射（ProcessEvent + FunctionParams）、CMake/Ninja、imgui 1.92。

## Global Constraints

- 所有 UFunction 调用在游戏线程（EngineTick 回调），不在 GUI 线程。
- is_valid 检查每步；null → 跳过（单只失败不影响其他）。
- 不缓存 UObject 跨帧。
- 构建在 VS x64 dev shell（PowerShell VsDevShell）。
- 验证：`format-check` + `PalworldEditor` + `PalworldEditorTests` + `PalworldEditorBaseResourceSharingTests` + `ctest`。
- 提交前 `git diff --check`；commit 结尾 `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`。
- 项目无反射/UI 单测；验证 = 构建 + ctest + 人工游戏内冒烟。

---

## Task 1: 后端（wantReviveTeam_ + game_thread_tick 复活逻辑）

**Files:**
- Modify: `mods/PalworldEditor/inc/mod/mod_core.hpp`
- Modify: `mods/PalworldEditor/src/mod/dllmain.cpp`

**Interfaces:**
- Produces: `PalworldEditorMod::wantReviveTeam_`（`std::atomic<bool>`）；`game_thread_tick` 消费它并调用 `SetPhysicalHealth(Healthful)`。

- [ ] **Step 1：mod_core.hpp 加 wantReviveTeam_ 成员**

在 `private` 区（`want_discover_` 附近）加：

```cpp
    /** @brief GUI 线程提交、game_thread_tick 消费的一次性复活请求。 */
    std::atomic<bool> wantReviveTeam_{false};
```

- [ ] **Step 2：dllmain.cpp game_thread_tick 加复活逻辑**

在 `game_thread_tick` 的「Discover」部分之前（`publish_skill_snapshot_if_dirty()` 之前），加复活处理。复活逻辑放在一个独立的 private 方法中更清晰。

先在 `mod_core.hpp` 的 private 区加方法声明（`process_grapple_work` 附近）：

```cpp
    /** @brief 遍历队伍槽位，复活所有处于死亡/濒死状态的帕鲁。 */
    auto revive_team_pals() -> void;
```

然后在 `dllmain.cpp`（`process_grapple_work` 定义之后）实现：

```cpp
auto PalworldEditorMod::revive_team_pals() -> void {
    auto* const holder = UObjectGlobals::FindFirstOf(STR("PalOtomoHolderComponentBase"));
    if (!pal_game::is_valid(holder)) {
        skillRuntimeSnapshot_.lastResult = "复活失败：未找到队伍 Holder。";
        skillSnapshotDirty_ = true;
        return;
    }

    const auto maxNum = pal_game::invoke<int>(holder, STR("GetMaxOtomoNum")).value_or(0);
    if (maxNum <= 0 || maxNum > 20) {
        skillRuntimeSnapshot_.lastResult = "复活失败：队伍槽位数异常。";
        skillSnapshotDirty_ = true;
        return;
    }

    int revivedCount = 0;
    for (int slotIndex = 0; slotIndex < maxNum; ++slotIndex) {
        // GetOtomoIndividualHandle(slotIndex) → Handle
        auto* const getHandleFunction =
            holder->GetFunctionByNameInChain(STR("GetOtomoIndividualHandle"));
        if (getHandleFunction == nullptr) {
            continue;
        }
        pal_game::FunctionParams handleParams{getHandleFunction};
        auto* const slotProp =
            CastField<FIntProperty>(getHandleFunction->FindProperty(FName(STR("SlotIndex"), FNAME_Find)));
        if (slotProp != nullptr) {
            slotProp->SetPropertyValueInContainer(handleParams.data(), slotIndex);
        }
        holder->ProcessEvent(getHandleFunction, handleParams.data());
        auto* const handleRetProp = CastField<FObjectPropertyBase>(
            getHandleFunction->FindProperty(FName(STR("ReturnValue"), FNAME_Find)));
        auto* const handle = handleRetProp != nullptr
            ? handleRetProp->GetObjectPropertyValue(
                  handleRetProp->ContainerPtrToValuePtr<void>(handleParams.data()))
            : nullptr;
        if (!pal_game::is_valid(handle)) {
            continue;
        }

        // Handle → TryGetIndividualParameter → Parameter
        auto* const parameter =
            pal_game::invoke<RC::Unreal::UObject*>(handle, STR("TryGetIndividualParameter"))
                .value_or(nullptr);
        if (!pal_game::is_valid(parameter)) {
            continue;
        }

        // IsDead → SetPhysicalHealth(Healthful)
        const auto isDead = pal_game::invoke<bool>(parameter, STR("IsDead")).value_or(false);
        if (!isDead) {
            continue;
        }
        auto* const setHealthFunction =
            parameter->GetFunctionByNameInChain(STR("SetPhysicalHealth"));
        if (setHealthFunction == nullptr) {
            continue;
        }
        pal_game::FunctionParams healthParams{setHealthFunction};
        auto* const healthProp = setHealthFunction->FindProperty(
            FName(STR("PhysicalHealth"), FNAME_Find));
        if (auto* const enumProp = CastField<FEnumProperty>(healthProp)) {
            enumProp->GetUnderlyingProperty()->SetIntPropertyValue(
                enumProp->ContainerPtrToValuePtr<void>(healthParams.data()), 0);
        }
        parameter->ProcessEvent(setHealthFunction, healthParams.data());
        ++revivedCount;
    }

    if (revivedCount > 0) {
        skillRuntimeSnapshot_.lastResult = "复活了 " + std::to_string(revivedCount) + " 只队伍帕鲁。";
    } else {
        skillRuntimeSnapshot_.lastResult = "队伍中没有需要复活的帕鲁。";
    }
    skillSnapshotDirty_ = true;
}
```

然后在 `game_thread_tick` 中消费 `wantReviveTeam_`（在 `publish_skill_snapshot_if_dirty()` 之前加）：

```cpp
    // Revive team pals
    if (wantReviveTeam_.exchange(false)) {
        revive_team_pals();
    }
```

dllmain.cpp 需要的额外 include（`<Unreal/Property/FEnumProperty.hpp>`、`<Unreal/Property/FIntProperty.hpp>`、`<common/game_reflection.hpp>`）。检查已有 includes，缺则补。

- [ ] **Step 3：format + 构建 + ctest**

```powershell
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor PalworldEditorTests PalworldEditorBaseResourceSharingTests
ctest --test-dir build --output-on-failure
```

Expected：全绿。若缺 include 导致编译错，按编译器提示补 `<Unreal/Property/FEnumProperty.hpp>` / `<Unreal/Property/FIntProperty.hpp>`。

- [ ] **Step 4：提交**

```bash
git add mods/PalworldEditor/inc/mod/mod_core.hpp mods/PalworldEditor/src/mod/dllmain.cpp
git commit -m "feat: add revive team pals backend (game_thread_tick)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Task 2: 前端（UI 按钮）+ 最终验证

**Files:**
- Modify: `mods/PalworldEditor/src/skills/skill_ui.cpp`

**Interfaces:**
- Consumes: `PalworldEditorMod::wantReviveTeam_`（Task 1 产出）。

- [ ] **Step 1：render_pal_editor 加「复活队伍帕鲁」按钮**

在 `render_pal_editor`（skill_ui.cpp）的 `ImGui::Separator()` 之后、技能编辑区之前（「选择当前帕鲁」按钮同一区域），加复活按钮：

```cpp
    // 复活队伍帕鲁（一次性操作，在 lifecycleReady 时可用）
    ImGui::BeginDisabled(pending || !lifecycleReady);
    {
        editor_ui::scoped_accent_button accent;
        if (ImGui::Button("复活队伍帕鲁")) {
            self->wantReviveTeam_.store(true, std::memory_order_release);
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("(一键复活所有死亡队伍帕鲁)");
```

放在 `ImGui::BeginDisabled(pending || !snapshot.worldAccessible)` / `EndDisabled` 的「刷新技能列表」之后。

- [ ] **Step 2：format + 构建 + ctest**

```powershell
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor PalworldEditorTests PalworldEditorBaseResourceSharingTests
ctest --test-dir build --output-on-failure
git diff --check
git status --short
```

Expected：三目标构建成功；ctest 2/2 通过；diff --check 无输出；工作树干净。

- [ ] **Step 3：人工游戏内冒烟（部署后）**

- 让队伍帕鲁死亡 → 点「复活队伍帕鲁」→ 确认复活（不再濒死/死亡）。
- 队伍无死亡帕鲁 → 点按钮 → 显示「队伍中没有需要复活的帕鲁」。
- 复活后帕鲁属性/技能/形态不变。

- [ ] **Step 4：提交**

```bash
git add mods/PalworldEditor/src/skills/skill_ui.cpp
git commit -m "feat: add revive team pals button to Pal editor tab

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```
