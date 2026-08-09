# 远程终端（Remote Palbox）设计

日期：2026-08-06。目标版本：PalworldEditor 1.6.11（合入 develop 后发布流程再定）。

## 背景与目标

玩家希望任意地点按自定义快捷键打开原生 Palbox UI，不必跑到基地的帕鲁终端旁边。
参考实现：

- [Palbox Anywhere（N 网 3927，UE4SS Lua）](https://www.nexusmods.com/palworld/mods/3927)：
  热键打开原生 Palbox UI；通过玩家持有的持久 `PalBaseCampModel` 数据解析 `BaseCampId` 与
  `OwnerMapObjectInstanceId`；地牢检测 `PalPlayerState.IsInStage()`、骑乘检测
  `PalPlayerController.IsRiding()`；修复了"原生异步 HUD push 被当作单次派发导致菜单开两次"。
- [Remote Palbox（N 网 1944，BPML）](https://www.nexusmods.com/palworld/mods/1944)：
  把交互对象移到玩家身边再调用原生交互函数；默认禁用地牢内打开（原版地牢内开终端有崩溃 bug）。

本项目以 UE4SS C++ 反射方式实现等价能力，不依赖 BPML/Lua。**硬约束：安全不崩溃、
尽最大程度不丢帧。**

## 范围

### 包含

- 自定义快捷键（默认 J，VK 0x4A）打开原生 Palbox UI；
- 4 个门控开关（地牢、骑乘、仅基地圈内、战斗中）默认值与 GUI 配置；
- 配置持久化到 `mods/PalworldEditor/remote_palbox.ini`；
- GUI"远程终端"区：改键、开关、"立即打开终端"测试按钮、状态与最近错误；
- 防双开、防连点、进行中保护。

### 不包含

- 聊天命令入口（F10 控制台不可用，交互全部走 UE4SS GUI）；
- 地牢内打开的稳定性：`DisableInDungeon` 关闭时可在地牢尝试打开，但原版存在崩溃 bug
  （参考 1944 的封禁原因），GUI 在该开关旁显示风险警告，本 mod 不承诺地牢内稳定；
- 专用服务器/主机端支持（沿用单人/本地房主基线）；
- 修改终端交互语义、动画或存档。

## 方案

采用**方案 A：直接调用原生 HUD Push 路径**（已获用户确认）：

1. 按键上升沿 → 门控检查 → 解析归属基地 → 构造
   `UPalHUDDispatchParameter_PalBox{BaseCampId, OwnerMapObjectInstanceId}` →
   `PalHUDService::Push(PalBox WidgetClass, param)` 打开原生 UI。

不采用方案 B（移动交互对象走完整交互状态机：副作用与异步状态机复杂度高）、
方案 C（Hook 交互参数解包：Hook 点需逆向、稳定性最差）。

## 架构与组件

遵循仓库既有"纯值层 + 游戏线程网关 + UI"模式：

```
inc/pal_remote_palbox/
  remote_palbox_config.hpp   ini 读写纯逻辑（可单测；未知键忽略、损坏回退默认）
  remote_palbox.hpp          纯值层：门控开关、按键上升沿状态机、基地选择策略
src/pal_remote_palbox/
  remote_palbox_runtime.cpp  游戏线程适配：EngineTick 轮询、反射门控、基地解析、HUD Push
  remote_palbox_ui.cpp       GUI"远程终端"区
```

模块间通过值类型 + 互斥锁快照通信（沿用 ImGui 回调只读快照、游戏线程写请求的现有惯例）。
跨帧不持有任何 UObject 指针或 Unreal 地址。

## 数据流（EngineTick，游戏线程）

1. **按键检测（每帧）**：`GetAsyncKeyState(配置VK)` 上升沿 + 游戏窗口前台检查 +
   300ms 防连点 + 进行中保护。每帧固定 2 次 WinAPI 调用，不接触任何游戏对象。
2. **世界就绪**：已进档、LoadMap 后、世界代次有效（复用 `world_session_state` 的现有
   世界代次判定）；否则静默忽略。
3. **门控（仅在上升沿）**：`PalPlayerState:IsInStage()`（默认禁）→
   `PalPlayerController:IsRiding()`（默认禁）→ `OnlyInsideBaseCircle`（默认关，
   开时检查玩家是否在某基地圈内）。
4. **基地解析（仅在上升沿）**：`PalBaseCampManager:GetBaseCampIds → TryGetModel`
   （资源分享已验证链路）→ 归属策略：**当前所在圈 → 包含玩家的圈 → 最近的已拥有基地**。
   均为本地玩家/本地公会，无跨玩家数据。
5. **取 PalBox 实例**：从目标基地的 `PalBaseCampModel` 持久数据取 `BaseCampId` 与
   `OwnerMapObjectInstanceId`（精确 getter 名称待游戏内验证，见"验证点"）。
6. **Push**：`PalHUDService:CreateDispatchParameterForK2Node` 创建
   `UPalHUDDispatchParameter_PalBox` → 写入两个 FGuid 字段 →
   `PalHUDService:Push(PalBox WidgetClass, param)`。
   param 对象生命周期由 HUD 服务持有，mod 不自行释放、不跨帧引用。
7. **验证**：Push 后查询 HUD 栈确认 PalBox widget 已打开；已打开时按键忽略（防双开）。

## 配置格式（remote_palbox.ini）

```
HotkeyVk=74            ; J 键（0x4A=74）
DisableWhileMounted=true    ; 默认：骑乘禁
DisableInDungeon=true       ; 默认：地牢禁
OnlyInsideBaseCircle=false  ; 默认：任意地点
DisableDuringCombat=false   ; 默认：战斗不禁
```

- 缺失/损坏 → 整段回退默认值（fail-closed 于配置，功能按默认值开放）；
- 未知键忽略；键位只接受合法 VK（1–255），非法回退 74；
- 启动时读取一次；GUI 修改后写回；写失败仅日志提示，不影响会话内生效。

## 安全与性能基线（硬约束）

### 安全（不崩溃）

- 全部反射调用：游戏线程单一入口、非拥有指针、`pal_game::is_valid` 校验、
  `CastField` 类型校验；
- **结构故障 → 域停用**：HUD 服务、`Push`、DispatchParameter 工厂、基地管理器等
  关键反射点任一不可用 → 本世界代次内停用远程终端域（GUI 显示原因），LoadMap 后
  重新探测；与现有属性/工作适应性等安全停用域同构且独立互锁；
- 触发失败（Push 成功但验证未开）→ 单次拒绝 + 日志 + GUI 状态行，不影响后续触发；
- 无跨帧 UObject 持有；param 对象生命周期归属 HUD 服务；
- ini 解析有界、fail-closed。

### 性能（尽最大程度不丢帧）

- 每帧开销固定为 2 次 WinAPI 调用（亚微秒级）；
- 全部游戏逻辑（门控反射、基地解析、参数构造、Push、验证）仅在按键上升沿
  一次性执行，不做逐帧反射、不引入常驻扫描、不使用 `FindAllOf`；
- 触发执行记录耗时：超 2ms 打日志留痕（一次性操作可接受）；连续 5 次超时 →
  域停用并提示检查；
- 上升沿内建 300ms 防连点 + 进行中保护；
- GUI 新增控件为静态渲染，无逐帧数据收集。

## 验证点（实现第一步需在游戏内确认）

1. **PalBox Widget Class 获取**：`EPalWidgetBlueprintType` 枚举中未直接命中 PalBox
   名称；候选：枚举值验证、`PalHUDService` 已注册 widget 表查询、或
   `ShowCommonUI(EPalWidgetBlueprintType, Parameter)` 路径。任一失败走下一候选。
2. **BaseCampId / OwnerMapObjectInstanceId 精确 getter**：`PalBaseCampModel` 持久
   数据的字段/getter 名称待 dump 与运行时确认；失败候选：从基地模型的地图对象
   实例列表解析 PalBox 实例。

两处均有 fallback 路径，不阻塞整体设计；验证结果回填本文档。

## 测试计划

### 纯值单测（PalworldEditorTests）

- ini 解析：合法/未知键/损坏/非法 VK/缺失文件回退默认；
- 上升沿状态机：首次按下触发、300ms 内忽略、抬起-按下重置、进行中保护；
- 基地选择策略：当前圈优先、包含玩家圈次之、最近已拥有兜底、无基地返回空。

### 游戏内验证清单

- 标题界面按 J 无反应且无崩溃；
- 进档后按 J 打开原生 Palbox UI，内容与本地终端一致；
- 地牢内/骑乘时默认禁用；战斗内默认可开；仅基地圈开关生效时基地外禁用；
- GUI 改键后立即生效、重启游戏后保留（ini 持久化）；
- 快速连按不双开；已打开时按键忽略；
- 没有基地/基地未加载时按 J：失败提示而非崩溃；
- LoadMap 后按键恢复可用、结构故障域停用提示恢复；
- 打开/关闭终端全程 FPS 无周期性波动；与资源共享、技能编辑、属性编辑互不干扰。

## 验收

- 所有单测通过；`format-check`、`git diff --check` 通过；
- 游戏内验证清单全绿；
- 无新增每帧反射路径（代码评审确认）。
