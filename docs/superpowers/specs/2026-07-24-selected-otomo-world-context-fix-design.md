# 当前待出战帕鲁 WorldContext 修复设计

## 背景

PalworldEditor v1.4.1 已不再使用 `Scan Pals` 结果中的跨帧 `UObject*` 作为技能编辑目标，
而是尝试通过以下反射链解析 Q/E 当前选中的待出战帕鲁：

1. `PalUtility::GetOtomoHolderComponent`
2. `PalOtomoHolderComponentBase::GetSelectedOtomoID`
3. `PalOtomoHolderComponentBase::GetOtomoIndividualHandle`
4. `PalIndividualCharacterHandle::TryGetIndividualParameter`

游戏内验证表明，GUI 能正常加载，但始终显示“尚未解析到待出战帕鲁”。UHTHeaderDump
确认上述四个函数名、参数顺序和返回类型与 Palworld 1.0 一致。失败发生在解析链入口：
当前代码把 `PalPlayerInventoryData` 直接作为 `WorldContextObject`，该对象不能可靠地为
`GetOtomoHolderComponent` 提供本地玩家世界上下文。

UE4SS 自带 UFunction Caller 会枚举 `PlayerController` 作为 `WorldContextObject` 候选，
因此本修复使用当前本地 `PlayerController` 作为 Pal 蓝图工具函数的世界上下文。

## 目标

- 正确解析本地玩家通过 Q/E 选中的下一只待出战帕鲁。
- 保持所有 Unreal 对象访问仅发生在游戏线程，且不跨帧缓存裸指针。
- 在解析失败时显示具体阶段，避免所有问题都退化成“尚未解析”。
- 不恢复可导致崩溃的扫描列表点击选取路径。
- 不改变物品、技能目录或技能写入接口的既有语义。

## 方案比较

### 方案一：选择本地 PlayerController（采用）

通过 `UObjectGlobals::FindAllOf("PlayerController")` 枚举控制器，调用
`/Script/Engine.Controller:IsLocalPlayerController` 选择本地控制器，并将其作为
`WorldContextObject` 传给 `GetOtomoHolderComponent`。

优点是符合 UE4SS 自带工具的 WorldContext 使用方式，能区分多人环境中的本地与远端玩家，
同时保留现有 Otomo 公开 UFunction 链。

### 方案二：直接扫描 Otomo Holder（不采用）

直接枚举 `PalOtomoHolderComponentBase` 派生实例可以绕过 WorldContext，但多人游戏中可能
选到其他玩家的 Holder，还需要额外解析组件 Owner，整体并不比方案一可靠。

### 方案三：Hook Q/E 切换事件（不采用）

Hook `SetSelectOtomoID` 或 `OnChangeOtomoSlot` 可以及时获知选择变化，但依赖蓝图实现和调用时序，
增加初始化、读档和网络同步风险，不适合作为本次最小修复。

## 设计

### 本地世界上下文解析

`pal_game::get_world_context()` 改为：

1. 查找 `/Script/Engine.Controller:IsLocalPlayerController`。
2. 枚举当前已加载的 `PlayerController` 对象。
3. 过滤空指针和无效对象。
4. 对候选对象调用 `IsLocalPlayerController()`。
5. 返回第一个本地控制器；没有可用本地控制器时返回空。

该函数每次调用都重新解析，不把控制器保存到全局或跨帧状态中。

### 分步诊断

`SelectedPalTarget` 增加解析状态，覆盖以下结果：

- `Success`
- `WorldContextUnavailable`
- `PalUtilityUnavailable`
- `GetHolderFunctionUnavailable`
- `HolderUnavailable`
- `GetSelectedFunctionUnavailable`
- `SelectedSlotUnavailable`
- `GetHandleFunctionUnavailable`
- `HandleUnavailable`
- `GetParameterFunctionUnavailable`
- `ParameterUnavailable`
- `ParameterClassUnavailable`
- `GetCharacterIdFunctionUnavailable`

`resolve_selected_otomo()` 在每个提前返回点写入对应状态。GUI 将状态映射成简短中文消息，
并保留刷新技能列表按钮。日志只在状态变化时输出，避免每帧重复刷屏。

### 数据与线程边界

- `PlayerController`、Holder、Handle 和 Parameter 都是当前游戏帧内的非拥有指针。
- `on_update()` 在游戏线程完成解析、读取和技能写入。
- ImGui 回调只读取标准库快照，并通过现有互斥锁与请求队列提交操作。
- `SelectedTargetState` 继续使用 `CharacterID` 和代数保护过期请求，不保存 Unreal 指针。

## 测试

纯 C++ 测试覆盖：

- 解析状态到用户消息的映射。
- 成功状态仍能触发目标切换和技能快照刷新。
- 失败状态会清空目标并拒绝旧目标的待处理编辑请求。
- 重复的相同失败状态不会产生重复状态变化。

构建验证执行：

```powershell
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor PalworldEditorTests
ctest --test-dir build --output-on-failure
git diff --check
```

游戏内端到端验证：

1. 队伍中至少放入两只帕鲁。
2. 使用 Q/E 切换待出战帕鲁。
3. 确认 GUI 中的 CharacterID 和主动/被动技能随选择变化。
4. 分别验证主动技能替换/清空和被动技能新增/替换/删除。
5. 确认不点击扫描列表也能完成全部操作，且不再崩溃。

## 非目标

- 本次不实现 UMG 或独立游戏内界面。
- 不重新引入全量帕鲁扫描作为编辑入口。
- 不修改技能数量上限、游戏合法性规则或网络同步策略。
- 不处理当前未跟踪的 `UHTHeaderDump.7z`。
