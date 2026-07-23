# 当前待出战帕鲁技能目标修复设计

## 背景

`PalworldEditor` 1.4.0 通过监听
`UPalIndividualCharacterParameter::GetPassiveSkillList` 的 `ProcessEvent` 调用来推断玩家
正在查看的帕鲁，并在没有查看目标时允许用户从 `Scan Pals` 结果中手动选择。

游戏的帕鲁状态界面并不保证通过 `ProcessEvent` 调用该函数，因此打开状态页或用 Q/E
切换当前战斗帕鲁时，mod 可能一直没有目标。手动扫描又把
`UPalIndividualCharacterParameter*` 裸指针保存到 GUI 缓存并跨帧使用；用户选择盒子中的
帕鲁后，技能目录还会把该帕鲁对象作为 `PalUIUtility` 的 `WorldContextObject`。盒中对象
不保证关联有效的 `UWorld`，这条路径会在首次加载技能本地化数据时造成游戏崩溃。

## 目标

- 编辑目标始终是玩家当前选中、下一次投掷时会出战的帕鲁。
- 游戏中使用 Q/E 切换帕鲁后，mod 在后续游戏帧自动切换目标并重读技能。
- 当前目标的被动技能与三个 `EquipWaza` 主动技能槽位可继续新增、替换、删除或清空。
- 打开帕鲁状态页、帕鲁盒子以及刷新技能目录都不会依赖无效 WorldContext 或跨帧扫描裸指针。
- 保持现有游戏线程执行、GUI 快照读取、FIFO 编辑与失败回滚语义。

## 非目标

- 不编辑帕鲁盒子中任意高亮的帕鲁。
- 不保留 `Scan Pals` 作为技能目标选择后备。
- 不修改 `MasteredWaza`、伙伴技能、存档协议或多人客户端权限模型。
- 不尝试通过 UI Widget 私有状态推断高亮对象。

## 方案选择

采用每个游戏帧轮询 `UPalOtomoHolderComponentBase` 的公开 UFunction 链：

1. 获取本地玩家可用且已经验证工作的 `PalPlayerInventoryData` 对象，作为稳定
   `WorldContextObject`。
2. 调用静态函数 `PalUtility:GetOtomoHolderComponent(WorldContextObject)`。
3. 在返回的 Holder 上调用 `GetSelectedOtomoID()`，得到当前待出战槽位索引。
4. 调用 `GetOtomoIndividualHandle(SlotIndex)`。
5. 在 Handle 上调用 `TryGetIndividualParameter()`，得到当前技能编辑目标。
6. 在 Parameter 上调用 `GetCharacterID()`，得到 GUI 显示用 Raw 物种 ID。

不采用 `SetSelectOtomoID`/`OnChangeOtomoSlot` Hook，因为初始化、读档、蓝图实现和网络同步
可能绕过单一 Hook 点。也不采用详情 UI Hook，因为详情高亮对象并不等价于下一只出战帕鲁。

## 组件与接口

### 当前目标解析

在 `inc/game/pal_game.hpp` 中新增只允许游戏线程调用的目标解析接口，并返回值语义快照：

```cpp
struct SelectedPalTarget {
    UObject* parameter{};
    std::string characterId;
};

[[nodiscard]] auto resolve_selected_otomo() -> SelectedPalTarget;
```

该函数内部只在一次调用期间保存 Holder、Handle 和 Parameter 裸指针。返回的
`parameter` 仍是非拥有观察指针，但调用方不会把扫描列表或 Handle 跨帧保存；每一帧都会从
玩家 Holder 重新解析。任何对象或 UFunction 缺失、槽位小于零、Handle 为空或 Parameter
无效时返回空结构。

物种名使用 `GetCharacterID()` 的返回值，不再把 `SaveParameter` 结构体地址解释为
`FName*`。

### 纯 C++ 目标状态

新增 `inc/skills/selected_target_state.hpp`，隔离可测试的目标变化语义：

```cpp
namespace skill_editor {
struct SelectedTargetObservation {
    SkillTarget target{};
    std::string name;
};

class SelectedTargetState {
public:
    [[nodiscard]] auto update(SelectedTargetObservation observation) -> bool;
    [[nodiscard]] auto current() const -> const SelectedTargetObservation&;

private:
    SelectedTargetObservation current_;
};
}
```

`update()` 仅当目标句柄变化时返回 `true`；同一目标的名称可以刷新但不触发技能状态重载。
空观察会清除目标，并在之前存在目标时返回 `true`。该类型不依赖 UE4SS，可由现有
`PalworldEditorTests` 直接覆盖。

### Mod 生命周期与 GUI

`PalworldEditorMod::on_update()` 每帧调用 `resolve_selected_otomo()`，把结果交给
`SelectedTargetState`。目标变化时：

- 清空旧目标的技能状态与结果消息；
- 重读新目标的被动/主动技能；
- 首次需要时加载技能目录；
- 发布新的 `SkillEditorSnapshot`。

移除以下状态和流程：

- `GetPassiveSkillList` ProcessEvent Hook；
- `lastViewedPal_`、`fnGetPSL_`、`suppressViewTracking_` 和 `ViewTrackingGuard`；
- `pal_selected_`、`pal_cache_`、`want_scan_pals_`；
- `Scan Pals` 按钮和可点击列表；
- `SkillTargetSource::viewed`/`selected` 区分。

GUI 统一显示“目标：名称（当前待出战）”。没有可用目标时提示玩家进入游戏并用 Q/E
选择一只队伍帕鲁。

### 技能目录 WorldContext

`PalSkillGateway::load_catalog()` 不再接收技能目标作为 WorldContext。它通过
`PalPlayerInventoryData` 获取稳定的本地玩家 WorldContext；获取失败时返回明确错误，不调用
`PalUIUtility:GetPassiveSkillName` 或 `GetWazaName`。

技能目录与具体帕鲁无关，因此成功后缓存复用；目标切换只重读技能状态。用户点击“刷新技能列表”
时才重新加载目录。

## 数据流与线程

```text
游戏线程 on_update
  -> PalPlayerInventoryData
  -> PalUtility:GetOtomoHolderComponent
  -> Holder:GetSelectedOtomoID
  -> Holder:GetOtomoIndividualHandle
  -> Handle:TryGetIndividualParameter
  -> SelectedTargetState::update
  -> 目标变化时读取技能
  -> mutex 保护下发布 SkillEditorSnapshot

GUI 线程
  -> 复制 SkillEditorSnapshot
  -> 渲染当前待出战帕鲁
  -> 把编辑请求压入 FIFO
```

所有 Unreal 反射调用继续只在游戏线程执行。GUI 线程不接触 UObject、Holder、Handle 或
Parameter。技能编辑请求仍携带创建时的目标句柄；游戏线程执行前必须用当帧重新解析的当前目标
进行一致性检查，目标已经切换时拒绝旧请求，避免误改下一只帕鲁。

## 错误处理

- WorldContext、Holder、槽位、Handle 或 Parameter 缺失：发布“当前没有可用的待出战帕鲁”，
  不调用技能反射函数。
- 技能目录加载失败：保留上一份成功目录并显示错误；没有历史目录时禁用新增/替换。
- 当前目标在请求排队后发生切换：拒绝请求并显示“目标帕鲁已切换，请重试”。
- 技能写入失败：保持现有操作后重读与回滚逻辑。
- 任何反射输出数组只在对应 `ProcessEvent` 返回后立即转换为标准库值，不跨帧保存。

## 测试

纯 C++ 测试新增以下回归场景：

1. 空状态首次观察有效目标时报告变化并保存目标。
2. 同一目标重复观察不报告变化，但允许更新名称。
3. Q/E 切换到另一目标时报告变化。
4. 当前目标消失时清空状态并报告变化。
5. 空状态重复观察空目标不报告变化。
6. 编辑请求的目标与当前重新解析目标不一致时被拒绝，不调用网关写入。

提交前执行：

```powershell
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor PalworldEditorTests
ctest --test-dir build --output-on-failure
git diff --check
```

## 游戏内验收

1. 只启用 `PalworldEditor`，进入已有队伍的存档。
2. 不打开帕鲁详情页，确认窗口显示当前下一只待出战帕鲁及其技能。
3. 用 Q/E 切换帕鲁，确认目标名称和技能列表同步变化。
4. 修改被动技能并确认操作后重读结果。
5. 装备、替换和清空主动技能并确认三个槽位顺序。
6. 打开状态页和帕鲁盒子，确认不崩溃且目标仍以战斗选择为准。
7. 在目标切换瞬间提交修改，确认旧请求被拒绝而不会修改新目标。
