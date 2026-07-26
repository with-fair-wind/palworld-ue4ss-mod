# 下一次召唤帕鲁目标解析修复设计

## 背景

Palworld 1.0 使用数字键切换队伍中的待召唤帕鲁，按 E 召唤当前高亮目标。当前
PalworldEditor 仍把这一状态描述为“Q/E 当前帕鲁”，并通过以下反射链解析目标：

1. 枚举 `PlayerController` 并调用 `IsLocalPlayerController`；
2. 调用 `PalUtility::GetOtomoHolderComponent`；
3. 在 `PalOtomoHolderComponentBase` 基类函数对象上调用
   `GetSelectedOtomoID`；
4. 用返回的槽位索引取得队伍帕鲁参数。

游戏内日志在状态 `1`、`4` 和 `6` 之间变化。它们分别对应未找到本地
`PlayerController`、未取得 Otomo Holder，以及选中槽位无效。

UHT dump 表明 `GetSelectedOtomoID` 是 `BlueprintImplementableEvent`。当前代码通过
`/Script/Pal.PalOtomoHolderComponentBase:GetSelectedOtomoID` 取得基类 `UFunction`，
可能绕过实际 Holder 蓝图派生类的实现。与此同时，本地控制器再经
`PalUtility::GetOtomoHolderComponent` 的间接链不能稳定取得 Holder。

## 目标语义

编辑目标必须满足以下定义：

- 属于本地玩家队伍；
- 是数字键切换后当前高亮的队伍槽位；
- 是下一次按 E 会召唤的帕鲁；
- 不得把已经存在于场景中的野生帕鲁作为候选；
- 场上已经存在其他帕鲁时，仍以队伍 UI 当前高亮目标为准。

例如：场上存在野生棉悠悠，而队伍 UI 当前高亮草莽猪时，编辑器必须解析草莽猪。

## 方案比较

### 方案一：从实际 Holder 实例解析本地队伍目标（采用）

枚举已加载的 `PalOtomoHolderComponentBase` 实例。对每个候选依次调用
`TryGetOwnerControlledPawn`、`Pawn::GetController` 和
`Controller::IsLocalPlayerController`，选出由本地玩家控制的 Holder。

随后从 Holder 实例的实际运行时类链中取得 `GetSelectedOtomoID`，确保调用蓝图派生
实现，再按返回槽位读取队伍中的 `IndividualHandle` 和参数对象。

该方案同时移除不稳定的世界上下文间接链，并修复蓝图函数分派问题。候选范围只有
Otomo Holder，不扫描场景角色，因此不会选择野生帕鲁。

### 方案二：保留 `PalUtility` 链，仅修复函数分派（不采用）

保留当前本地控制器和 `GetOtomoHolderComponent` 解析，只把
`GetSelectedOtomoID` 改为从实际对象类链查找。

该方案能修复状态 `6`，但不能解释或消除状态 `1` 和 `4`，不足以解决已复现问题。

### 方案三：Hook 数字键或槽位切换事件（不采用）

Hook 输入或 `SetSelectOtomoID`、`OnChangeOtomoSlot` 等事件，维护 mod 自己的槽位状态。

该方案依赖输入映射、蓝图实现和初始化时序；读档、重连及其他非输入触发的槽位变化
仍需额外同步，不适合作为主解析来源。

## 架构

目标解析仍只在 `on_update()` 所在游戏线程执行，不跨线程保存任何 Unreal 对象。

解析链调整为：

```text
FindAllOf("PalOtomoHolderComponentBase")
  -> Holder.TryGetOwnerControlledPawn()
  -> Pawn.GetController()
  -> Controller.IsLocalPlayerController()
  -> local Holder
  -> Holder runtime class: GetSelectedOtomoID()
  -> Holder.GetOtomoIndividualHandle(selected slot)
  -> Handle.TryGetIndividualParameter()
  -> FPalInstanceID.InstanceId + CharacterID
```

GUI 与游戏线程之间继续只传递 `InstanceId`、显示名称、目标代数和请求参数。每次技能
写入前重新运行同一解析链，并校验目标代数和 `InstanceId`，避免数字键切换后旧请求
写入错误帕鲁。

## 组件变更

### `pal_game.hpp`

- 新增本地 Holder 候选解析，并用它替换目标解析中的
  `PlayerController` 枚举和 `PalUtility::GetOtomoHolderComponent` 路径。
- 对 `GetSelectedOtomoID` 使用 Holder 实例的
  `GetFunctionByNameInChain`，从实际蓝图派生类开始查找。
- 其余 Handle、Parameter、GUID 和 CharacterID 解析保持现有边界。
- 不缓存 Holder、Pawn、Controller、Handle 或 Parameter 指针。

`get_world_context()` 仍作为技能本地化的公共入口，但不再独立枚举
`PlayerController`。它复用同一个本地 Holder 解析器；Holder 是绑定到当前游戏世界的
`UActorComponent`，可直接作为蓝图 `WorldContextObject`。这样目标解析和技能目录不会
使用两套互相矛盾的“本地玩家”发现逻辑。

### `selected_target_state.hpp`

- 调整解析状态，使错误能够区分：
  - 未发现 Holder 候选；
  - 未找到本地玩家 Holder；
  - Holder 所有者 Pawn 或 Controller 不可用；
  - 实际类链中没有 `GetSelectedOtomoID`；
  - 当前选中槽位无效。
- 保留目标身份、代数和过期请求拦截语义。

### `dllmain.cpp`

- 将“Q/E 当前帕鲁”文案改成“数字键当前高亮、下一次按 E 召唤的队伍帕鲁”。
- 继续只在解析状态变化时输出常规日志。
- 失败日志补充 Holder 候选数量、候选实际类名和失败阶段；避免每帧刷屏。

### `pal_skills.cpp`

- 技能目录继续调用 `get_world_context()`，但得到的是本地玩家 Holder，而不是不稳定的
  `PlayerController` 枚举结果。
- 世界上下文不可用时，错误文案改成“未找到本地玩家队伍 Holder”，与目标解析诊断一致。

### 文档

- README、AGENTS.md 和 CLAUDE.md 中涉及 Q/E 切换的说明改为数字键切换与 E 召唤语义。
- 游戏内验证步骤使用“队伍高亮目标”和“下一次按 E 召唤目标”，不绑定具体数字键，
  以兼容玩家自定义按键。

## 错误处理

未解析到目标时：

- 不自动退回扫描场景中的 `PalCharacter`；
- 不使用第一个 Holder 或第一个队伍槽位作为猜测；
- 取消已经确认的编辑目标；
- 清空待处理技能请求；
- GUI 显示具体失败阶段；
- 保持 mod、物品功能和技能目录刷新可用。

如果同一帧发现多个本地 Holder，解析视为歧义并拒绝选择，同时记录候选实际类名。
这样不会在分屏、重连残留或多人环境中静默编辑错误玩家的帕鲁。

## 测试策略

### 纯 C++ 回归测试

- 从多个 Holder 候选中只选择有效且归属于本地控制器的候选；
- 没有本地 Holder 时返回明确失败；
- 多个本地 Holder 时拒绝歧义结果；
- 目标 GUID 变化会取消已确认目标；
- 旧代数的编辑请求不会进入写入回调；
- 新解析状态都具有可操作的中文诊断。

运行时类链的 `UFunction` 分派依赖 UE4SS/Unreal，纯 C++ 测试不能完整模拟。实现时会把
候选选择策略保持为可注入谓词的纯逻辑，并用单元测试覆盖；实际蓝图派生函数分派通过
构建和游戏内端到端验证。

### 构建验证

```powershell
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor PalworldEditorTests
ctest --test-dir build --output-on-failure
git diff --check
```

### 游戏内验证

1. 队伍中放入草莽猪和至少另一只帕鲁。
2. 场景中保留一只野生棉悠悠。
3. 用数字键把队伍 UI 高亮目标切换到草莽猪，但暂不按 E。
4. 点击“选择当前帕鲁”，确认目标为草莽猪而不是野生棉悠悠。
5. 按 E，确认实际召唤目标与编辑器目标一致。
6. 用数字键切换到另一只队伍帕鲁，确认旧目标立即失效并要求重新确认。
7. 在目标切换前后提交技能修改，确认旧代数请求被拒绝且游戏不崩溃。
8. 检查 UE4SS 日志，确认不再出现状态 `1/4/6` 循环，失败时能看到明确阶段。

## 非目标

- 不编辑野生、敌对、据点工作或场景扫描得到的帕鲁。
- 不把当前已经召唤在场的帕鲁强制作为编辑目标。
- 不 Hook 键盘输入或绑定固定数字键。
- 不改变主动/被动技能写入协议和回滚规则。
