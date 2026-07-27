# 帕鲁属性编辑器 Design

**目标版本：** PalworldEditor 1.6.6（在 1.6.5 被动技能分类之后）
**日期：** 2026-07-27

## 目标

在现有"选择当前帕鲁"流程中新增三项数值编辑：等级、个体值（HP/攻击/防御）、亲密度。
所有写入只在游戏线程、按需执行（用户点击"应用"或目标切换时），不引入逐帧任务、后台扫描或额外线程。

## 范围与数值策略

| 属性 | 底层字段 | 类型 | 可编辑范围 | 说明 |
|---|---|---|---|---|
| 等级 | `SaveParameter.Level` | uint8 | 1–80 | 不超过游戏满级，避免经验表空段 |
| 个体值·HP | `SaveParameter.Talent_HP` | uint8 | 0–255 | 突破游戏 100 上限 |
| 个体值·攻击 | `SaveParameter.Talent_Shot` | uint8 | 0–255 | 远程攻击 IV；`Talent_Melee` 为 `Transient` 不存盘，不编辑 |
| 个体值·防御 | `SaveParameter.Talent_Defense` | uint8 | 0–255 | 突破游戏 100 上限 |
| 亲密度 | `FriendshipPoint`（rank 0–10 派生） | int32 | rank 0–10 | 上升走 `AddFriendShip`；下降直接写点数 |

**生效时机：** 写入存档后持久化；当前帕鲁面板数值在重新召唤或重载存档后刷新。不调用属性重算入口（游戏未公开可调用入口）。

## 非目标

- 不修改 `MasteredWaza`、物种兼容性或伙伴技能（主动技能"不正常效果"是单独的已知限制，不在本设计范围）。
- 不追求写入后实时刷新当前帕鲁面板（接受重召唤/重载后生效）。
- 不编辑 `Talent_Melee`（`Transient`，不存盘）。
- 不引入逐帧任务、后台线程或全局 `FindAllOf` 扫描。

## 架构

独立于技能管线（不触碰刚发布的被动分类代码），复用既有安全基础设施：目标解析、GUID 重校验、LoadMap 清空、GUI 只读互斥快照。

### 模块

- **纯 C++ 领域层** `inc/pal_stats/pal_stat_editor.hpp`（无 Unreal 依赖，可单测）：
  - `PalStatValues`：`std::optional<int>` 的 `level`/`talentHp`/`talentShot`/`talentDefense`/`friendshipRank`。
  - `PalStatEditRequest`：目标 `PalStatValues` + `targetGeneration`/`worldGeneration`。
  - `PalStatSnapshot`：当前 `level`/`talentHp`/`talentShot`/`talentDefense`/`friendshipRank`/`friendshipPoint`，供 GUI 显示。
  - 纯值 clamp/校验函数：按上表范围裁剪；非法值拒绝；空 `optional` 表示"不改该项"。
- **网关适配层** `src/pal_stats.cpp`（或并入 `pal_skills.cpp`），只在游戏线程执行：
  - `read_stats(target) -> PalStatSnapshot`。
  - `apply_stat_edit(target, request) -> bool`。
- **dllmain 集成**：`statQueue_`（与 `skillQueue_` 平行的小队列）、`statRuntimeSnapshot_`；在 `game_thread_tick` 技能队列之后消费；`render_pal_editor` 渲染；`begin_world_transition` 清空。请求只携带代次，不传 `UObject*`。

### 数据流

1. GUI 在互斥锁下读 `statRuntimeSnapshot_` 显示当前值。
2. 用户改 `InputInt`、点"应用属性修改" → 构建 `PalStatEditRequest`（带代次）入 `statQueue_`。
3. 游戏线程 `game_thread_tick`：取请求 → 重新解析当前选中帕鲁 `parameter` → 校验 target/world 代次与 GUID → clamp → 写字段 → 读回 → 发布新 `statRuntimeSnapshot_`。
4. `begin_world_transition`（LoadMap）清空 `statQueue_`、撤销写权限；不保留 `UObject*`。

### 快照刷新时机（保证空闲零开销）

`statRuntimeSnapshot_` 只在以下事件重算，**不**搭便车于 2 秒技能目录定时器：

- 目标帕鲁切换（选中确认）。
- 一次 `apply_stat_edit` 完成后读回。

空闲帧、目录定时器触发帧都不读取属性 → 空闲反射开销为零。

## 读写机制

目标对象为技能编辑器已有的 `PalIndividualCharacterParameter`；其 `SaveParameter`（`FPalIndividualCharacterSaveParameter`）是直接 UPROPERTY 成员。导航方式与现有 `StackCount` 一致：`parameter → "SaveParameter"(FStructProperty) → 字段(FByteProperty/FIntProperty) → Get/SetPropertyValueInContainer`。

| 属性 | 读取 | 写入 |
|---|---|---|
| 等级 | `GetLevel()` UFunction | 直接写 `SaveParameter.Level`（uint8，clamp 1–80） |
| 个体值×3 | 导航 `SaveParameter.Talent_HP/Shot/Defense`（无 getter） | 直接写同字段（uint8，0–255） |
| 亲密度 | `GetFriendshipRank()` + `GetFriendshipPoint()` | 上升 `AddFriendShip(delta, true)`（触发委托、应用被动）；下降直接写 `FriendshipPoint` |

**亲密度 rank↔point：** 下降需 rank→点数阈值。实现时先尝试从 `PalDatabaseCharacterParameter` 读取阈值；不可得则用已知 datamine 阈值表（带 fallback 与日志警告）。这是三项中不确定性最高的一项。

## 帧时间保证

- 所有反射按需、单次、游戏线程；无逐帧任务、无后台线程、无 `FindAllOf` 扫描。
- 空闲帧零反射开销。
- 比被动技能分类更轻（分类有 8 ID/tick、500µs 预算的后台增量任务；属性编辑无后台任务）。
- Apply 那一帧仅若干 `ProcessEvent`/属性写（微秒级），单帧、不持续。

## UI

帕鲁编辑区（主动/被动技能之后）新增"属性修改"区块：选中帕鲁后显示当前 等级 / 三项个体值 / 亲密度 rank；每项一个 `InputInt`（带范围 clamp）；一个"应用属性修改"按钮入队。禁用条件与技能编辑一致（未选帕鲁 / 运行时未就绪 / 世界切换中 / 有待处理请求）。

## 测试

- 纯 C++（`PalworldEditorTests`）：等级/IV/亲密度的范围 clamp、非法值拒绝、空 `optional` 跳过、请求代次校验。
- DLL 构建。
- 游戏内验证：写入后重召唤/重载确认数值改变；空闲 10 秒无反射日志刷屏；LoadMap 后队列清空、重新选择前不可写入。

## 风险与待定

- **亲密度阈值来源**：三项中不确定性最高；计划阶段优先查 `PalDatabaseCharacterParameter` 是否暴露阈值数组。
- **嵌套结构体解析**：依赖 `SaveParameter` 字段反射（均为 UPROPERTY，应有反射，无需 `MemberVariableLayout`）；游戏更新后字段名/偏移变化需同步。首个游戏内验证点。
- **生效确认**：必须验证"重召唤/重载后数值确实改变"，否则需回退或补充触发手段。
