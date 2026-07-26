# PalworldEditor 事件驱动据点资源池重构设计

## 背景与根因

PalworldEditor 1.5.3 的资源共享在制作与建造预览 Hook 中按 1 秒缓存周期重新执行完整发现：

- 全局枚举 `PalBaseCampModuleItemStorage`；
- 全局枚举 `PalItemContainer`；
- 逐个读取所有已发现箱子的 `ItemSlotArray`；
- 在真实请求到达后，才临时扩展据点模块和 `PalItemContainerMultiHelper` 的容器数组。

游戏内日志显示每次预览缓存重建约耗时 32–39 毫秒，因此建造预览会产生明显丢帧。另一方面，真实联合只在
`StartProduction` 或 `RequestBuild_ToServer` 才建立；当材料全部位于其他据点时，原版流程会在更早的材料
校验阶段拒绝请求，导致真实联合从未建立。

## 目标

- 仅支持单人世界和本地房主，不支持远程客户端或专用服务器。
- 不再进行逐秒 UObject 全局扫描或逐秒箱子槽位扫描。
- 进入建造模式或制作界面后，在第一次材料校验之前建立同公会资源联合。
- 同一次玩家主动操作中的预览、许可判断和真实扣料使用相同的容器集合。
- 材料全部位于远端据点、分散在多个据点或本地据点部分拥有时，都能由 Palworld 原生逻辑校验并扣除。
- 不移动物品，不直接修改 `ItemSlotArray`、`StackCount` 或存档。
- 退出操作会话、关闭开关、LoadMap 或卸载时恢复原始容器关系。
- 不让普通箱子 UI、帕鲁运输、自动生产、食物箱或其他公会的箱子永久共享。
- 不跨帧保存裸 `UObject*`、`FProperty*`、数组地址或槽位地址。

## 非目标

- 不实现远程客户端显示或专用服务器协议。
- 不实现修理材料共享。
- 不共享公会箱、食物箱、玩家背包以外的特殊容器、掉落物容器或未登记场景容器。
- 不逆向复制 IntegratedStorage 的二进制实现，也不依赖其安装。
- 不在这一轮引入版本绑定的机器码签名或原生函数 Detour。

## 方案比较

### 方案 A：操作会话范围的事件驱动资源联合

世界就绪后通过 Palworld 管理器 API 建立纯值容器目录。箱子或据点结构事件只设置脏标记，由下一个
EngineTick 合并处理；低频校准用于补偿遗漏事件。进入建造模式或制作界面时建立一次联合，整个操作会话复用，
退出或空闲后恢复。

优点：

- 不在每帧预览中扫描或修改数组；
- 联合在早期校验前已经存在，真实请求不会被提前拦截；
- 影响范围限制在玩家主动建造或制作期间；
- 可继续让 Palworld 原生逻辑负责校验、扣除、复制与保存；
- 只使用 UHT dump 中存在的管理器 API、属性和 UFunction。

缺点：

- 制作界面没有明确的关闭 UFunction，需要空闲租约作为恢复兜底；
- 仍需游戏内验证原版各入口确实读取被联合的模块和主背包资源助手。

### 方案 B：世界存续期永久联合

进入世界后立即把所有同公会普通箱子长期追加到每个据点模块，直到 LoadMap 或关闭开关。

优点是实现简单，并且所有早期校验天然可见。缺点是可能让帕鲁运输、自动生产或其他读取
`ContainerInfos` 的系统也跨据点工作，超出既定范围，因此不采用。

### 方案 C：保留请求期联合并补齐更多预览 Hook

继续在 `CanStartProduction`、`CollectItemInfoEnableToUseMaterial`、`IsEnableBuild` 等高频函数中修正返回值，
等真实请求到达时再建立联合。

优点是数组修改窗口短。缺点是需要维护更多易变的函数参数布局，且仍可能遗漏更早的原生直接调用；高频 Hook
也容易重新引入帧时间尖峰，因此不采用。

## 选定架构

采用方案 A：**纯值目录 + 事件驱动失效 + 操作会话联合 + 8 秒低频校准**。

### 角色安全门

每次世界初始化只在以下条件成立时启用资源能力：

- `PalUtility:IsServer(WorldContextObject)` 返回 `true`；
- `PalUtility:IsDedicatedServer(WorldContextObject)` 返回 `false`；
- 可以解析本地玩家及其公会 GUID；
- 可以解析 `PalBaseCampManager` 和 `PalMapObjectManager`；
- 当前世界代次仍可访问。

任何条件失败都保持原版行为并在 GUI 快照中报告原因。

### 纯值资源目录

新增不依赖 Unreal 的目录与调度状态：

- 当前世界代次和公会 GUID；
- 据点 GUID；
- 据点存储模块完整对象名；
- 普通箱子容器 GUID；
- 箱子所属地图物体实例 GUID；
- 稳定顺序和去重结果；
- 结构目录版本、最后成功校准时间和诊断信息。

目录不保存 Unreal 对象指针。所有对象在当次 EngineTick 或 UFunction 回调中重新解析并立即丢弃。

### 无全局扫描发现

结构校准使用以下已确认的 Palworld 1.0 UHT 接口：

1. `PalUtility:GetBaseCampManager`；
2. `PalBaseCampManager:GetBaseCampIds`；
3. `PalBaseCampManager:TryGetModel`；
4. 从 `PalBaseCampModel.ModuleArray` 中读取 `PalBaseCampModuleItemStorage`；
5. 读取该模块原生 `ContainerInfos`；
6. `PalUtility:GetMapObjectManager`；
7. `PalMapObjectManager:FindConcreteModel(OwnerMapObjectConcreteModelInstanceId)`；
8. `PalMapObjectConcreteModelBase:GetItemContainerModule`；
9. `PalMapObjectItemContainerModule:GetContainer`。

校准不会调用 `FindAllOf("PalBaseCampModuleItemStorage")`、
`FindAllOf("PalItemContainer")` 或 `FindAllOf("PalItemContainerMultiHelper")`，也不读取箱子槽位来计算预览。

### 事件与低频校准

结构事件 Hook 的回调只设置标准库调度标志，不在回调中扫描：

- `PalBaseCampModuleItemStorage:OnRep_ContainerInfos`；
- `PalBaseCampModuleItemStorage:OnAvailableConcreteModel_ServerInternal`；
- `PalBaseCampModuleItemStorage:OnNotAvailableConcreteModel_ServerInternal`；
- `PalBaseCampModel:OnRep_ModuleArray`。

多个事件在下一个 EngineTick 合并为一次校准。世界就绪后立即调度初始校准；失败时以 1 秒退避重试。成功后
最多每 8 秒执行一次无全局扫描的结构校准，以补偿未经过上述 UFunction 的服务器内部变化。

### 操作会话与联合租约

建造会话：

- `PalBuilderComponent:OnStartBuildingMode_Local` post-hook 请求建造租约并尝试建立联合；
- `PalBuilderComponent:IsExistsMaterialForBuildObject` pre-hook 只在联合尚未建立时执行一次兜底建立，并刷新租约；
- `PalBuilderComponent:OnEndBuildingMode_Local` post-hook 释放建造租约；
- 若结束事件遗漏，LoadMap、关闭开关和卸载仍强制恢复。

制作会话：

- `PalUIConvertItemModel:Initialize` post-hook 请求制作租约；
- `CalcMaxProductableNum`、`CanStartProduction` 和 `StartProduction` 刷新租约活动时间，并且只在联合尚未建立时执行
  一次兜底建立；
- 连续 1.5 秒没有制作界面活动且没有建造租约时释放制作租约；
- `StartProduction` pre-hook 必须重新确认联合、世界代次和公会一致。

联合只建立一次，不在同一菜单的每次预览调用中追加和恢复数组。
建立动作直接发生在上述游戏线程 UFunction 回调中；事件驱动的结构校准仍只在 EngineTick 执行。若初始目录尚未
生成，首次建立允许通过管理器 API 同步执行一次无全局扫描校准。

### 联合范围

建立联合时：

1. 重新解析目录中的每个据点模块和普通箱子；
2. 验证目录的世界代次、公会和容器解析完整性；
3. 对每个同公会据点模块，追加缺失的普通箱子 `ContainerInfo` 副本；
4. 只扩展本地 `PalPlayerInventoryData.InventoryMultiHelper`，不枚举或修改其他 helper；
5. 调用必要的原版通知函数，使后续材料检查读取新集合；
6. 记录纯值恢复账本。

恢复账本记录模块完整对象名、原生容器 GUID 序列和本次注入 GUID 的次数。恢复不再要求注入项仍是精确数组尾部：
它按记录的注入次数删除匹配项，同时保留会话期间由游戏新增的其他原生元素。恢复后重新读取并验证没有遗留注入。

如果目录变化发生在联合活动期间，下一个 EngineTick 先恢复旧联合，再校准原生目录，最后按仍有效的租约重新建立
联合。这样不会把上一次注入项误认为原生箱子。

### 预览与真实扣料

资源联合建立后，Palworld 原生的制作和建造材料检查直接看到同一组容器，不再由 mod 每秒读取槽位并覆写预览
结果。真实制作与建造也沿用同一联合，物品扣除仍由 Palworld 完成。

旧实现中的以下机制删除：

- 1 秒 `PreviewCacheGate`；
- 玩家与据点槽位数量聚合；
- `CalcMaxProductableNum` 返回值改写；
- `IsExistsMaterialForBuildObject` 返回值改写；
- 每次请求重新执行的全局发现；
- 建造请求后的 0.75 秒临时联合窗口；
- 全局 `PalItemContainerMultiHelper` 枚举。

如果游戏内验证证明某个原版检查不读取会话联合，则只针对该入口增加纯值结果适配，不恢复全局扫描或逐调用数组
联合。

## 错误处理

- 目录校准失败：保留旧的已验证纯值目录；若当前没有可用目录则不开启联合。
- 容器解析不完整：不建立部分联合，保持原版行为。
- 联合追加失败：立即按本次账本回滚；回滚验证失败则本世界禁用资源共享。
- 恢复时对象已经销毁：视为该对象不再持有注入；其他仍存在对象继续验证恢复。
- 世界代次、公会或服务器角色变化：立即恢复并清空目录、租约和待处理事件。
- Hook 缺失：结构事件 Hook 可以由 8 秒校准降级；建造/制作会话入口缺失则只禁用对应能力。
- 自己触发的容器通知由重入标志抑制，不重复调度校准。

## 纯 C++ 测试

新增或调整测试覆盖：

- 世界就绪触发初始校准，成功后 8 秒才再次校准；
- 多个结构事件合并为一次下一 Tick 校准；
- 失败校准按 1 秒退避，不在每帧重试；
- 建造租约在开始/结束之间保持联合需求；
- 制作租约在 1.5 秒无活动后到期；
- 两类租约重叠时只在最后一个释放后恢复；
- 世界代次变化立即清空租约和目录；
- 注入计划按公会过滤、去重和稳定排序；
- 恢复按注入次数移除匹配项并保留会话期间新增的原生元素；
- 二次应用联合保持幂等；
- 关闭状态不注册会话 Hook、不校准、不建立联合；
- 能力评估区分必需会话入口与可降级结构事件入口。

## 构建与游戏内验证

构建验证：

```powershell
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor PalworldEditorTests PalworldEditorBaseResourceSharingTests
ctest --test-dir build --output-on-failure
git diff --check
```

游戏内测试必须禁用 IntegratedStorage、UBIM Lite 及其他修改相同资源路径的 mod：

1. 关闭共享时，对比未启用该功能的帧率与工厂/建造界面响应。
2. 开启共享并进入世界，不打开建造界面也能在初始校准后显示据点与普通箱子数量。
3. 反复打开建造和制作界面，日志中不再出现逐秒全局扫描或 30 毫秒级预览重建。
4. 当前据点材料为零、另一据点材料充足时，预览允许操作并成功扣除远端材料。
5. 两个据点分别持有部分材料时，合计足够可以操作；合计不足时不发生扣除。
6. 新建和拆除普通箱子后，目录通过事件在下一 Tick 更新；遗漏事件时最多 8 秒校准。
7. 建造/制作会话结束后，帕鲁运输、自动生产和箱子 UI 仍保持原版据点范围。
8. 关闭开关、退出世界、重进存档和卸载时均恢复联合且不崩溃。
9. 连续冷启动并进入主界面，世界安全门之前不执行资源反射。

## 兼容性说明

这一实现仅依赖 Palworld 1.0 UHT dump 中可见的 UFunction 和反射属性，不引入机器码地址。Palworld 或 PalSchema
更新后，需要重新验证函数存在性、参数属性名、`ContainerInfos` 结构和事件触发行为。游戏内端到端测试通过前，
GUI 应把能力标记为实验性，不宣称支持专用服务器或远程客户端。
