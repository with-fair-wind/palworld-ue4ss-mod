# 爪钩枪无冷却 Design

**日期：** 2026-07-27
**目标版本：** 在当前 `main`（1.6.4）之上递增一个次版本；具体号在合并时确定（与被动分类、属性编辑器各自独立 PR，合并顺序决定最终版本号）。

## 目标

在 UE4SS GUI 增加一个「爪钩枪无冷却」开关。开启时翻转游戏自带的
`UPalDebugSetting.bDisableGrapplingCoolDown`，使爪钩枪射击无冷却；关闭时恢复游戏默认。

## 范围

- 一个 on/off 二元开关（**不**提供可调 CD 数值/倍率）。
- 开启 = 无冷却；关闭 = 恢复默认。
- 偏好持久化到 `config.ini`；启动与世界就绪时重应用。

## 非目标

- 不提供可调 CD 数值（不碰 `PalGrapplingGunModule.NearCoolTimeRate`、不 Hook `CanHitGrapplingTarget`）。
- 不影响其他武器、帕鲁技能或伙伴技能的冷却。
- 不针对单个存档或单只帕鲁——`bDisableGrapplingCoolDown` 是全局调试开关。

## 架构

**写入机制：** 游戏线程用 `UObjectGlobals::FindFirstOf(STR("PalDebugSetting"))` 取得
`UPalDebugSetting` 实例，通过 `FBoolProperty::SetPropertyValueInContainer` 写
`bDisableGrapplingCoolDown`。无 Hook、无逐帧工作，与现有 `StackCount`/属性编辑同模式。
（等价访问 `PalUtility:GetPalDebugSetting` 静态函数；优先用更简单的 `FindFirstOf`。）

**应用时机**（沿用资源共享开关的「原子请求 + 游戏线程应用 + 世界就绪重应用」模式）：

- GUI 勾选/取消 → 设置原子 `requestedGrappleNoCooldown_` 并置 `grappleSettingDirty_`。
- `game_thread_tick` 检测到 dirty → 应用（写 flag）；`PalDebugSetting` 暂不可用时本轮跳过、下轮再试。
- 世界就绪 / LoadMap 后**重应用**（debug flag 可能被游戏重置）。

**持久化：** `config.ini` 新增独立小节：

```ini
[GrapplingHook]
NoCooldown=true
```

`on_program_start` 读取（与既有 `[BaseResourceSharing]` 同文件、独立小节）；缺失或无效时安全回退为关闭。
配置读写复用既有 settings 模块的最小扩展，不新建子系统。

**GUI：** 主窗口一个 checkbox「爪钩枪无冷却」，勾选状态反映持久化偏好；与既有「据点资源共享」开关同区。

**结构：** 网关函数 `set_grapple_no_cooldown(bool)` 加到 `inc/game/pal_game.hpp`（游戏反射层）；
`dllmain.cpp` 加两个原子量（`requestedGrappleNoCooldown_`、`grappleSettingDirty_`）+ 一段 GUI + config 读写。
不新建模块、不新增请求队列。

## 帧时间

- 仅在用户切换开关或世界就绪重应用时，单次游戏线程写一个 bool（微秒级）；空闲帧零开销。
- 无逐帧任务、无后台线程、无扫描。

## 风险

- `bDisableGrapplingCoolDown` 是**全局**调试开关（影响整个游戏，不止当前存档）——符合本功能定位。
- 可能在世界切换后被游戏重置 → 已用「世界就绪重应用」覆盖。
- 是否**实时生效**需游戏内确认（调试开关一般逐次读取，极可能即时；若非即时则需补充触发）。

## 测试

- DLL 构建；`PalworldEditorTests` 不受影响（纯反射开关无新纯逻辑可单测）。
- 游戏内验证：勾选 → 爪钩枪可连发无 CD；取消 → 恢复正常 CD；退出并重进存档 → 偏好保持且生效；
  切换世界后仍生效（重应用）；空闲等待无逐帧日志刷屏。
