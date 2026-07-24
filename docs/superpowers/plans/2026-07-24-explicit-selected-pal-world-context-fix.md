# Explicit Selected Pal and WorldContext Fix Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 使用本地 `PlayerController` 正确解析 Q/E 当前帕鲁，并且只有点击“选择当前帕鲁”后才启用主动/被动技能编辑。

**Architecture:** 游戏线程每帧解析本地控制器、Otomo Holder、当前槽位和个体参数，但只把纯值 `FPalInstanceID.InstanceId` 与目标代数发布给 GUI。GUI 显式提交选择请求；技能修改请求只携带代数，游戏线程在执行前重新解析并校验 GUID，再把当前帧的参数指针注入执行器。任何 Q/E 目标变化或解析失败都会取消选择、清空技能快照并拒绝旧请求。

**Tech Stack:** C++23、UE4SS Unreal 反射、ImGui、CMake/Ninja、CTest。

## Global Constraints

- Palworld 目标版本为 1.0，运行时使用 UE4SS Experimental 与匹配的 PalSchema。
- 所有 Unreal API 只允许在 `on_unreal_init()` 之后的游戏线程调用。
- 不跨帧缓存或在线程之间传递 `UObject*`，包括编码成整数的指针。
- 队伍中相同 `CharacterID` 的帕鲁必须通过 `FPalInstanceID.InstanceId` 区分。
- Q/E 目标变化后立即取消选择；用户必须再次点击“选择当前帕鲁”。
- 未选择目标时清空技能状态并禁用全部技能编辑控件。
- 不恢复 `Scan Pals` 点击列表作为编辑目标的旧路径。
- 不修改未跟踪的 `UHTHeaderDump.7z`。
- 保持 `.clang-format` 的 Allman 大括号、4 空格缩进和 120 列限制。

---

## File Map

- `mods/PalworldEditor/inc/skills/selected_target_state.hpp`
  - 保存纯值个体 GUID、手动确认状态、目标代数和过期请求校验。
- `mods/PalworldEditor/inc/skills/skill_editor_service.hpp`
  - 为 GUI 技能编辑请求增加目标代数；保留仅供游戏线程使用的瞬时 `SkillTarget`。
- `mods/PalworldEditor/inc/game/pal_game.hpp`
  - 解析本地 `PlayerController`、Q/E 当前帕鲁、`FPalInstanceID.InstanceId` 和分步失败状态。
- `mods/PalworldEditor/src/dllmain.cpp`
  - 消费“选择当前帕鲁”请求、管理失效状态、发布无指针快照并渲染按钮。
- `mods/PalworldEditor/tests/skill_editor_tests.cpp`
  - 覆盖手动确认、GUID 切换、目标代数和瞬时指针注入。
- `README.md`、`AGENTS.md`、`CLAUDE.md`
  - 记录 v1.4.2 手动选择交互、WorldContext 和验证步骤。

---

### Task 1: 以纯值 GUID 和代数实现显式目标确认

**Files:**
- Modify: `mods/PalworldEditor/inc/skills/selected_target_state.hpp`
- Modify: `mods/PalworldEditor/inc/skills/skill_editor_service.hpp`
- Test: `mods/PalworldEditor/tests/skill_editor_tests.cpp`

**Interfaces:**
- Produces: `skill_editor::TargetIdentity`
- Produces: `skill_editor::SelectedTargetObservation`
- Produces: `skill_editor::SelectedTargetResolutionStatus`
- Produces: `skill_editor::SelectedTargetState::confirm()`
- Produces: `skill_editor::SelectedTargetState::invalidate_if_changed()`
- Produces: `skill_editor::SelectedTargetState::matches()`
- Produces: `SkillEditRequest::targetGeneration`
- Produces: `SkillEditQueue::clear() -> std::size_t`
- Produces: `apply_if_target_is_current(request, state, observation, transientTarget, apply)`

- [ ] **Step 1: 写入显式确认和 GUID 切换的失败测试**

用以下测试替换旧的指针身份测试，并把新测试加入 `main()`：

```cpp
auto identity(const std::uint32_t value) -> skill_editor::TargetIdentity {
    return {.instanceId = {value, value + 1, value + 2, value + 3}};
}

void test_target_requires_explicit_confirmation() {
    skill_editor::SelectedTargetState state;
    const skill_editor::SelectedTargetObservation observed{
        .identity = identity(10),
        .name = "Boar",
    };

    CHECK(!state.is_selected());
    CHECK(!state.invalidate_if_changed(observed));
    CHECK(!state.is_selected());

    CHECK(state.confirm(observed));
    CHECK(state.is_selected());
    CHECK(state.current().identity == observed.identity);
    CHECK(state.current().name == "Boar");
    CHECK(state.generation() == 1);
}

void test_qe_change_invalidates_even_for_same_character_id() {
    skill_editor::SelectedTargetState state;
    CHECK(state.confirm({.identity = identity(10), .name = "Boar"}));
    const auto selectedGeneration = state.generation();

    CHECK(state.invalidate_if_changed({.identity = identity(20), .name = "Boar"}));
    CHECK(!state.is_selected());
    CHECK(state.generation() == selectedGeneration + 1);
    CHECK(!state.current().is_valid());
}

void test_resolution_status_has_actionable_message() {
    CHECK(skill_editor::resolution_status_message(
              skill_editor::SelectedTargetResolutionStatus::worldContextUnavailable) ==
          "未找到本地 PlayerController");
    CHECK(skill_editor::resolution_status_message(
              skill_editor::SelectedTargetResolutionStatus::holderUnavailable) ==
          "未取得当前玩家的 Otomo Holder");
    CHECK(skill_editor::resolution_status_message(
              skill_editor::SelectedTargetResolutionStatus::success)
          .empty());
}
```

- [ ] **Step 2: 运行测试并确认 RED**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorTests
```

Expected: 编译失败，提示 `TargetIdentity`、`is_selected`、`confirm`、
`SelectedTargetResolutionStatus` 或 `resolution_status_message` 尚不存在。

- [ ] **Step 3: 为编辑请求增加目标代数**

在 `SkillEditRequest` 中保留 `target` 作为游戏线程临时字段，并增加：

```cpp
/** @brief GUI 提交请求时观察到的已确认目标代数。 */
std::uint64_t targetGeneration{};
```

删除未被生产代码使用的 `SkillEditQueue::contains_target()`；队列不得再根据瞬时指针查询请求。
增加：

```cpp
auto clear() -> std::size_t {
    const std::lock_guard lock(mutex_);
    const auto discarded = requests_.size();
    requests_.clear();
    return discarded;
}
```

测试应先压入三个请求，确认 `clear()` 返回 3、随后 `size()` 为 0 且 `try_pop()` 返回空。

- [ ] **Step 4: 实现纯值目标身份、解析状态和手动确认状态机**

在 `selected_target_state.hpp` 中实现以下公开形状：

```cpp
struct TargetIdentity {
    std::array<std::uint32_t, 4> instanceId{};

    [[nodiscard]] auto is_valid() const -> bool {
        return std::ranges::any_of(instanceId, [](const auto value) { return value != 0; });
    }

    auto operator==(const TargetIdentity&) const -> bool = default;
};

struct SelectedTargetObservation {
    TargetIdentity identity;
    std::string name;

    [[nodiscard]] auto is_valid() const -> bool {
        return identity.is_valid();
    }
};

enum class SelectedTargetResolutionStatus {
    success,
    worldContextUnavailable,
    palUtilityUnavailable,
    getHolderFunctionUnavailable,
    holderUnavailable,
    getSelectedFunctionUnavailable,
    selectedSlotUnavailable,
    getHandleFunctionUnavailable,
    handleUnavailable,
    getParameterFunctionUnavailable,
    parameterUnavailable,
    parameterClassUnavailable,
    getPalIdFunctionUnavailable,
    individualIdUnavailable,
    getCharacterIdFunctionUnavailable,
};
```

`resolution_status_message()` 返回规格中的简短中文诊断。`SelectedTargetState` 使用以下接口：

```cpp
[[nodiscard]] auto confirm(SelectedTargetObservation observation) -> bool;
[[nodiscard]] auto invalidate_if_changed(const SelectedTargetObservation& observation) -> bool;
auto invalidate() -> void;
[[nodiscard]] auto is_selected() const -> bool;
[[nodiscard]] auto generation() const -> std::uint64_t;
[[nodiscard]] auto current() const -> const SelectedTargetObservation&;
[[nodiscard]] auto matches(std::uint64_t generation,
                           const SelectedTargetObservation& observation) const -> bool;
```

规则：

- `confirm()` 只接受有效 GUID，成功时保存观测、设置已选择并增加代数。
- 未选择状态下的 `invalidate_if_changed()` 不会自动确认目标。
- 已选择时，空观测或不同 GUID 会清空目标并增加代数。
- 相同 GUID 只更新展示名称，不增加代数。
- `matches()` 同时要求已选择、代数相同、当前观测有效且 GUID 相同。

- [ ] **Step 5: 让执行辅助函数只在游戏线程注入瞬时指针**

把 `apply_if_target_is_current` 改为：

```cpp
template <typename Apply>
[[nodiscard]] auto apply_if_target_is_current(
    const SkillEditRequest& request, const SelectedTargetState& state,
    const SelectedTargetObservation& observation, const SkillTarget transientTarget,
    Apply&& apply) -> std::optional<SkillEditResult> {
    if (transientTarget == 0 || !state.matches(request.targetGeneration, observation)) {
        return std::nullopt;
    }

    auto executableRequest = request;
    executableRequest.target = transientTarget;
    return std::invoke(std::forward<Apply>(apply), executableRequest);
}
```

- [ ] **Step 6: 更新过期请求测试**

验证 GUI 请求中的 `target` 为 0，回调只收到游戏线程注入的当帧指针：

```cpp
void test_stale_generation_never_reaches_apply_callback() {
    skill_editor::SelectedTargetState state;
    const skill_editor::SelectedTargetObservation observed{
        .identity = identity(10),
        .name = "Boar",
    };
    CHECK(state.confirm(observed));

    int applyCalls = 0;
    skill_editor::SkillTarget appliedTarget = 0;
    const auto apply = [&](const skill_editor::SkillEditRequest& request) {
        ++applyCalls;
        appliedTarget = request.target;
        return skill_editor::SkillEditResult{
            .status = skill_editor::SkillEditStatus::succeeded,
        };
    };

    const auto accepted = skill_editor::apply_if_target_is_current(
        {.targetGeneration = state.generation()}, state, observed, 0x2000, apply);
    CHECK(accepted.has_value());
    CHECK(applyCalls == 1);
    CHECK(appliedTarget == 0x2000);

    const auto stale = skill_editor::apply_if_target_is_current(
        {.targetGeneration = state.generation() + 1}, state, observed, 0x2000, apply);
    CHECK(!stale.has_value());
    CHECK(applyCalls == 1);
}
```

- [ ] **Step 7: 运行测试并确认 GREEN**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorTests
ctest --test-dir build --output-on-failure
```

Expected: `PalworldEditor.SkillEditor` 1/1 通过。

- [ ] **Step 8: 提交纯状态改动**

```powershell
git add mods/PalworldEditor/inc/skills/selected_target_state.hpp `
        mods/PalworldEditor/inc/skills/skill_editor_service.hpp `
        mods/PalworldEditor/tests/skill_editor_tests.cpp
git commit -m "feat: require explicit selected Pal confirmation"
```

---

### Task 2: 从本地 PlayerController 解析当前帕鲁和个体 GUID

**Files:**
- Modify: `mods/PalworldEditor/inc/game/pal_game.hpp`
- Test: `mods/PalworldEditor/tests/skill_editor_tests.cpp`

**Interfaces:**
- Consumes: `skill_editor::SelectedTargetObservation`
- Consumes: `skill_editor::SelectedTargetResolutionStatus`
- Produces: `pal_game::get_world_context() -> UObject*`
- Produces: `pal_game::resolve_selected_otomo() -> SelectedPalTarget`

- [ ] **Step 1: 增加本地候选选择规则的失败测试**

在纯状态头文件中声明可测试的候选选择辅助函数，并先写测试：

```cpp
void test_first_valid_local_candidate_is_selected() {
    const std::array candidates{1, 2, 3, 4};
    const auto selected = skill_editor::find_local_candidate(
        candidates, [](const int value) { return value != 1; },
        [](const int value) { return value == 3 || value == 4; });
    CHECK(selected.has_value());
    CHECK(*selected == 3);
}
```

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorTests
```

Expected: 编译失败，提示 `find_local_candidate` 不存在。

- [ ] **Step 2: 实现最小候选选择辅助函数**

在 `selected_target_state.hpp` 中实现：

```cpp
template <std::ranges::input_range Range, typename IsValid, typename IsLocal>
[[nodiscard]] auto find_local_candidate(const Range& candidates, IsValid&& isValid,
                                        IsLocal&& isLocal)
    -> std::optional<std::ranges::range_value_t<Range>> {
    for (const auto& candidate : candidates) {
        if (std::invoke(isValid, candidate) && std::invoke(isLocal, candidate)) {
            return candidate;
        }
    }
    return std::nullopt;
}
```

重新构建测试，Expected: 1/1 通过。

- [ ] **Step 3: 改用本地 PlayerController 作为 WorldContext**

`get_world_context()` 执行：

```cpp
std::vector<UObject*> controllers;
UObjectGlobals::FindAllOf(STR("PlayerController"), controllers);
auto* const isLocalFunction = UObjectGlobals::StaticFindObject<UFunction*>(
    nullptr, nullptr, STR("/Script/Engine.Controller:IsLocalPlayerController"));
if (isLocalFunction == nullptr) {
    return nullptr;
}

const auto selected = skill_editor::find_local_candidate(
    controllers, [](UObject* candidate) { return is_valid(candidate); },
    [isLocalFunction](UObject* candidate) {
        struct Params {
            bool ReturnValue{};
        } params;
        candidate->ProcessEvent(isLocalFunction, &params);
        return params.ReturnValue;
    });
return selected.value_or(nullptr);
```

更新函数注释，明确返回本地 `PlayerController`，不再声称返回背包对象。

- [ ] **Step 4: 返回分步解析状态和纯值身份**

`SelectedPalTarget` 改为：

```cpp
struct SelectedPalTarget {
    UObject* parameter{};
    skill_editor::SelectedTargetObservation observation;
    skill_editor::SelectedTargetResolutionStatus status{
        skill_editor::SelectedTargetResolutionStatus::worldContextUnavailable};
};
```

`resolve_selected_otomo()` 的每个提前返回点设置对应状态。取得参数对象后调用：

```cpp
struct PalInstanceId {
    FGuid PlayerUId;
    FGuid InstanceId;
    FString DebugName;
};
struct GetPalIdParams {
    PalInstanceId ReturnValue;
} getPalIdParams;
parameter->ProcessEvent(getPalIdFunction, &getPalIdParams);
```

若 `InstanceId.is_valid()` 为假，返回 `individualIdUnavailable`。否则复制：

```cpp
observation.identity.instanceId = {
    getPalIdParams.ReturnValue.InstanceId.A,
    getPalIdParams.ReturnValue.InstanceId.B,
    getPalIdParams.ReturnValue.InstanceId.C,
    getPalIdParams.ReturnValue.InstanceId.D,
};
```

`GetCharacterID()` 成功后写入 `observation.name`，最终状态设为 `success`。

增加必要包含：

```cpp
#include <Unreal/FString.hpp>
#include <Unreal/UnrealCoreStructs.hpp>
#include <skills/selected_target_state.hpp>
```

- [ ] **Step 5: 更新技能目录 WorldContext 错误文本**

在 `mods/PalworldEditor/src/pal_skills.cpp` 中把：

```cpp
catalog.error = "PalPlayerInventoryData world context is unavailable";
```

改为：

```cpp
catalog.error = "Local PlayerController world context is unavailable";
```

- [ ] **Step 6: 构建运行时适配层**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditor PalworldEditorTests
ctest --test-dir build --output-on-failure
```

Expected: DLL 构建成功，CTest 1/1 通过。

- [ ] **Step 7: 提交反射解析改动**

```powershell
git add mods/PalworldEditor/inc/game/pal_game.hpp `
        mods/PalworldEditor/inc/skills/selected_target_state.hpp `
        mods/PalworldEditor/src/pal_skills.cpp `
        mods/PalworldEditor/tests/skill_editor_tests.cpp
git commit -m "fix: resolve selected Pal from local controller"
```

---

### Task 3: 增加“选择当前帕鲁”按钮并移除 GUI 指针传递

**Files:**
- Modify: `mods/PalworldEditor/src/dllmain.cpp`

**Interfaces:**
- Consumes: `pal_game::resolve_selected_otomo()`
- Consumes: `SelectedTargetState::confirm()`
- Consumes: `SelectedTargetState::invalidate_if_changed()`
- Consumes: `SkillEditRequest::targetGeneration`
- Produces: `wantSelectCurrentPal_`
- Produces: 无 `UObject*`/`SkillTarget` 的 `SkillEditorSnapshot`

- [ ] **Step 1: 修改技能快照和 GUI 请求字段**

`SkillEditorSnapshot` 删除 `SkillTarget target`，增加：

```cpp
std::uint64_t targetGeneration{};
bool targetSelected{};
skill_editor::SelectedTargetResolutionStatus resolutionStatus{
    skill_editor::SelectedTargetResolutionStatus::worldContextUnavailable};
```

新增：

```cpp
std::atomic<bool> wantSelectCurrentPal_{false};
std::uint64_t skillUiGeneration_{};
```

删除 `skillUiTarget_`。所有 GUI 创建的 `SkillEditRequest` 使用：

```cpp
.targetGeneration = snapshot.targetGeneration,
```

不得在 GUI 线程设置 `.target`。

- [ ] **Step 2: 在游戏线程实现手动确认和切换失效**

在 `on_update()` 中：

1. 每帧调用 `resolve_selected_otomo()`。
2. 只有 `status == success` 时把 `selectedPal.observation` 作为有效观测。
3. 已选择时调用 `invalidate_if_changed()`；变化或失败时清空技能状态，并通过
   `skillQueue_.clear()` 一次性丢弃所有旧请求。
4. 消费 `wantSelectCurrentPal_`；成功解析时调用 `confirm()` 并读取技能状态。
5. 选择失败时不启用编辑，把具体解析状态发布给 GUI。
6. `wantRefreshSkillCatalog_` 独立于目标选择处理，不得自动确认目标。

技能请求执行使用：

```cpp
editResult = skill_editor::apply_if_target_is_current(
    *request, selectedTarget_, currentObservation,
    reinterpret_cast<skill_editor::SkillTarget>(selectedPal.parameter),
    [this](const skill_editor::SkillEditRequest& executableRequest) {
        return skill_editor::execute_skill_edit(skillGateway_, executableRequest);
    });
```

校验失败时调用 `selectedTarget_.invalidate()`，并返回：

```cpp
"当前帕鲁已变化，请重新点击“选择当前帕鲁”。"
```

若目标变化时丢弃了队列请求，则 `lastResult` 显示：

```cpp
"当前帕鲁已变化，已取消待处理的技能修改。"
```

- [ ] **Step 3: 发布无指针 GUI 快照**

持有 `skillSnapshotMutex_` 时写入：

```cpp
skillSnapshot_.targetGeneration = selectedTarget_.generation();
skillSnapshot_.targetSelected = selectedTarget_.is_selected();
skillSnapshot_.palName =
    selectedTarget_.is_selected() ? selectedTarget_.current().name : std::string{};
skillSnapshot_.resolutionStatus = selectedPal.status;
```

目标失效时将 `state` 清空。仅当已选择且本帧解析 GUID 仍匹配时，才用
`selectedPal.parameter` 调用 `read_state()`。

只在 `resolutionStatus` 变化时输出一条日志，不得每帧刷屏。

- [ ] **Step 4: 渲染显式选择按钮和禁用状态**

`render_pal_editor()` 在复制快照后：

```cpp
if (self->skillUiGeneration_ != snapshot.targetGeneration) {
    self->skillUiGeneration_ = snapshot.targetGeneration;
    reset_skill_editor_ui(self);
}

if (ImGui::Button("选择当前帕鲁")) {
    self->wantSelectCurrentPal_.store(true);
}
ImGui::SameLine();
if (ImGui::Button("刷新技能列表")) {
    self->wantRefreshSkillCatalog_.store(true);
}
```

未选择时显示：

```cpp
ImGui::TextDisabled("请在游戏中用 Q/E 选择帕鲁，然后点击“选择当前帕鲁”。");
```

解析状态非成功时额外显示：

```cpp
ImGui::TextColored(ImVec4(1.0F, 0.45F, 0.35F, 1.0F), "解析状态：%s",
                   skill_editor::resolution_status_message(snapshot.resolutionStatus).data());
```

`snapshot.targetSelected == false` 时在分隔线后立即返回，不渲染主动/被动技能编辑区。

- [ ] **Step 5: 更新生命周期注释和版本号**

将 `ModVersion`、加载日志和窗口标题从 `1.4.1` 更新为 `1.4.2`。更新注释：

- 每帧解析只用于检测 Q/E 当前目标。
- 只有选择请求成功后才发布可编辑技能快照。
- GUI 与游戏线程不传递参数对象地址。

- [ ] **Step 6: 格式化并构建**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target format
cmake --build --preset ninja-msvc-x64 --target PalworldEditor PalworldEditorTests
ctest --test-dir build --output-on-failure
```

Expected: DLL 构建成功，CTest 1/1 通过。

- [ ] **Step 7: 提交 GUI 和线程交接改动**

```powershell
git add mods/PalworldEditor/src/dllmain.cpp
git commit -m "feat: add explicit current Pal selection"
```

---

### Task 4: 同步文档并执行完整验证

**Files:**
- Modify: `README.md`
- Modify: `AGENTS.md`
- Modify: `CLAUDE.md`

**Interfaces:**
- Consumes: PalworldEditor v1.4.2 的最终交互和运行时约束。
- Produces: 用户安装、操作和游戏内验证说明。

- [ ] **Step 1: 更新用户文档**

同步以下内容：

- 版本号改为 `1.4.2`。
- 说明先用 Q/E 选中待出战帕鲁，再点击“选择当前帕鲁”。
- 说明 Q/E 切换会清空编辑目标，必须再次点击按钮。
- 说明“刷新技能列表”只刷新目录，不选择目标。
- 说明目标解析使用本地 `PlayerController` 和 `FPalInstanceID.InstanceId`。
- 游戏内检查加载日志 `PalworldEditor loaded (v1.4.2)`。

- [ ] **Step 2: 自检文档与代码没有旧语义**

Run:

```powershell
rg -n "1\.4\.1|PalPlayerInventoryData world context|自动解析.*编辑|尚未解析到待出战帕鲁" `
    README.md AGENTS.md CLAUDE.md mods/PalworldEditor
```

Expected: 无旧版本号、旧 WorldContext 错误文本或自动编辑目标文案。

- [ ] **Step 3: 执行仓库规定的完整验证**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor PalworldEditorTests
ctest --test-dir build --output-on-failure
git diff --check
```

Expected:

- `format-check` 成功。
- `PalworldEditor` 和 `PalworldEditorTests` 构建成功。
- CTest 1/1 通过。
- `git diff --check` 无输出。

- [ ] **Step 4: 提交文档**

```powershell
git add README.md AGENTS.md CLAUDE.md
git commit -m "docs: explain explicit current Pal selection"
```

- [ ] **Step 5: 游戏内端到端验证清单**

构建并部署后逐项验证：

1. UE4SS 日志出现 `PalworldEditor loaded (v1.4.2)`。
2. 未点击按钮时，主动/被动技能编辑区不显示。
3. 点击“选择当前帕鲁”后显示 CharacterID 和技能列表。
4. Q/E 切换后列表立即清空，编辑区禁用。
5. 重新点击按钮后可以新增、替换、删除被动技能。
6. 可以装备、替换、清空主动技能。
7. 两只相同物种帕鲁之间切换仍会取消选择。
8. 解析失败时显示具体失败阶段，不再只有笼统提示。
9. 不点击扫描列表即可完成操作，且不发生崩溃。

游戏内验证结果必须由实际运行 Palworld 得出；仅有 CTest 和 DLL 构建不能声称运行时问题已修复。
