# PalworldEditor 据点资源共享性能修复设计

## 背景

PalworldEditor 1.5.0 的第一版据点资源共享会为 13 条制作、建造和辅助 UI
`UFunction` 注册 Hook。共享开启后，每个预览调用都会重新枚举全部据点模块、
物品容器和资源助手，临时扩展数组，再在调用结束时恢复。工厂和建造菜单会在一帧内
多次调用这些函数，因此打开界面时出现持续卡顿。

关闭共享后卡顿明显减轻，但 Hook 仍保持注册，并且资源状态仍在每个 EngineTick
重新格式化和发布，所以相较完全没有该功能仍存在固定开销。

## 目标

- 共享关闭时不保留资源 `UFunction` Hook，不扫描据点，也不在状态无变化时每帧发布快照。
- 共享开启时，工厂和建造菜单的高频预览不修改任何 Unreal 容器数组。
- 预览材料数量允许最多约 1 秒延迟；真实制作和建造始终重新读取实时容器。
- 实际消费仍由 Palworld 完成，不直接写 `ItemSlotArray` 或 `StackCount`。
- 保留世界代次、LoadMap、精确恢复和按世界失败关闭保护。
- 提供低频耗时诊断，支持游戏内复核性能，而不产生逐调用日志洪泛。

## 非目标

- 不合并箱子界面。
- 不支持远程客户端或专用服务器。
- 不增加修理、运输、食物箱或自动生产共享。
- 不保证预览数字在一秒缓存窗口内立即反映刚发生的物品变化。

## 方案选择

采用“按需 Hook + 纯值预览缓存 + 实时消费联合”。

不采用只缓存 UObject 发现结果但仍为每次 UI 调用扩展数组的方案，因为高频数组修改和
恢复仍可能造成帧时间尖峰。不采用只保留消费 Hook 的方案，因为原版菜单可能在预览阶段
误判材料不足并阻止用户操作。

## Hook 生命周期

资源 Hook 只在以下条件同时满足时注册：

- 用户开关已开启；
- 当前世界可访问；
- EngineTick 与 LoadMap 生命周期回调已成功注册。

开关关闭、LoadMap 开始或 mod 卸载时：

1. 若存在活动消费联合，先恢复并验证；
2. 注销全部资源 `UFunction` Hook；
3. 清空预览缓存和请求状态；
4. 保留用户配置偏好和无 UObject 的 GUI 快照。

重新进入世界且开关仍开启时，在游戏线程重新解析并注册 Hook。注册失败只影响对应能力，
不会猜测函数或参数布局。

## 必要入口

常驻清单缩减为四条：

- 制作预览：`PalUIProductSettingModel:CalcMaxProductableNum`；
- 制作消费：`PalUIConvertItemModel:StartProduction`；
- 建造预览：`PalBuilderComponent:IsExistsMaterialForBuildObject`；
- 建造消费：`PalNetworkPlayerComponent:RequestBuild_ToServer`。

移除九条 `uiConsistency` 辅助 Hook。菜单中的共享可制作数量和是否可建造由两个顶层预览
入口修正；不再拦截通用物品计数/收集函数，避免影响其他游戏系统。

## 预览缓存

预览缓存只保存标准库值：

- 当前世界代次；
- 当前公会 GUID；
- 以物品 Raw ID 为键的非负总数量；
- 生成时间；
- 最近一次重建耗时和错误。

第一次预览请求、缓存超过 1 秒、世界代次变化或缓存被显式失效时，在游戏线程执行一次
完整、只读的同公会普通仓储发现和数量聚合。缓存有效时，后续高频预览只执行标准库查找。

以下事件使缓存失效：

- 制作或建造请求开始；
- 开关变化；
- LoadMap 前后；
- 容器解析不完整；
- 预览所需物品 ID 或数量非法。

缓存不得保存 `UObject*`、数组地址、槽位地址或 `FProperty*`。

### 制作预览

在 `CalcMaxProductableNum` post-hook 中：

1. 从当前产品模型调用 `GetRequiredMaterialInfos(OneUnit=true)`；
2. 合并重复物品需求并拒绝负数或无效 ID；
3. 用缓存数量计算全局最大可制作数；
4. 只允许把原版返回值提高为 `max(vanilla, sharedMaximum)`，不得降低。

### 建造预览

在 `IsExistsMaterialForBuildObject` post-hook 中：

1. 仅当原版返回 `false` 时读取本次 `BuildObjectData`；
2. 合并最多四组材料的重复 ID；
3. 只有全部需求都能由缓存满足时才把结果改为 `true`；
4. 任何参数布局或读取失败都保留原版结果。

## 实时消费

`StartProduction` 和 `RequestBuild_ToServer` 不使用预览缓存。

- 制作：pre-hook 重新发现实时容器并打开联合，post-hook 立即恢复和验证。
- 建造：服务器请求 pre-hook 重新发现实时容器并打开联合，最多保留 0.75 秒，
  随后在 EngineTick 恢复和验证。

消费请求开始后立即使预览缓存失效。发现、联合或恢复失败时保留原版行为，并将对应能力
锁定为本世界不可用。

## 状态发布与诊断

资源快照采用 dirty 标记。只有以下状态变化才重新格式化并加锁发布：

- 开关、世界代次或可访问性变化；
- Hook 能力变化；
- 缓存重建结果或据点/容器统计变化；
- 运行时错误变化。

不再在每个 EngineTick 无条件调用 `publish_snapshot()`。

记录以下耗时，但同类日志每次状态变化或消费请求最多输出一条：

- 预览缓存重建耗时；
- 制作实时发现、联合与恢复耗时；
- 建造实时发现、联合与恢复耗时。

## 测试

纯 C++ 测试覆盖：

- 关闭状态不需要 Hook；
- 开启且世界可访问时需要 Hook；
- LoadMap 时立即撤销 Hook 需求；
- 1 秒内缓存命中、到期后重建；
- 世界代次变化使缓存失效；
- 制作最大数量不会低于原版值；
- 重复需求合并和材料不足判断；
- dirty 快照只在状态变化时消费一次。

构建验证：

```powershell
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor PalworldEditorTests PalworldEditorBaseResourceSharingTests
ctest --test-dir build --output-on-failure
git diff --check
```

游戏内验证：

1. 共享关闭时对比未加载资源功能的帧率和工厂菜单响应。
2. 开启共享并反复打开工厂/建造菜单，确认不再持续卡顿。
3. 在另一据点改变材料后，确认预览在约 1 秒内更新。
4. 点击制作或建造，确认实际实时扣料且没有持续卡顿。
5. 关闭开关和反复 LoadMap，确认 Hook 注销、联合恢复且不崩溃。
