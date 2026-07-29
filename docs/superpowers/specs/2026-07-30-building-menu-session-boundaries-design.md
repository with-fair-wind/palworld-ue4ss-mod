# 建筑菜单资源会话边界修正设计

**日期：** 2026-07-30  
**状态：** 已确认

## 问题

跨据点资源目录和材料联合能够成功建立，但建筑菜单仍存在两个可稳定复现的问题：

1. 第一次按 B 时材料数量已经包含共享材料，建筑图标却保持不可建造；关闭后第二次按 B 才可建造。
2. 在一个据点打开过建筑菜单后传送到另一个据点，第一次按 B 仍复用旧据点会话；必须触发一次建筑操作并再次打开菜单，共享才恢复。

运行日志证明首次按 B 已经完成目录发现和当前据点模块联合，但没有执行建筑资格刷新。代码同时存在一条不可达条件：
建造联合的消费面是 `currentBaseModule`，而 `Setup` 后处理仍要求 `playerHelper` 和 Helper 数组账本。

传送问题来自建造菜单关闭缺少明确会话释放入口。同一种 `building` 操作再次进入时，
`ForegroundMaterialSession::acquire` 返回 `refreshed`，快速路径直接复用旧联合，不重新确认当前据点。

## 目标

- 第一次按 B 即可正确显示并选择共享材料足够的建筑。
- 关闭建筑菜单、传送到另一据点后，第一次按 B 即建立新据点联合。
- 建造提交前仍验证当前据点和联合序列，拒绝过期或跨据点操作。
- 不新增 EngineTick 工作、定时器、后台线程、全局 UObject 扫描或逐物品扫描。
- 高频建筑列表和材料资格 Hook 不执行目录发现、据点解析或数组修改。

## 事件边界

### 打开菜单

`PalUIBuildModel:OnOpenMenu` pre-hook 是首选会话起点。在原版首次生成建筑资格数据前：

1. 同步执行一次有界目录发现；
2. 通过本地 Pawn 的 `InsideBaseCampCheckComponent` 解析当前据点；
3. 若存在绑定其他据点的旧联合，先按账本恢复；
4. 向当前据点普通仓储模块临时注入其他同公会据点的普通箱子登记；
5. 保存当前世界代次、当前据点 GUID 和恢复账本。

### Setup 兜底和资格刷新

`PalUIInGameMainMenuBuildModel:Setup` pre-hook 使用同一个“菜单边界获取”操作：

- 尚无活动联合时建立联合；
- 活动联合属于当前世界和当前据点时复用；
- 当前据点变化时恢复旧联合并建立新联合。

`Setup` post-hook 只在当前建造会话尚未刷新且活动联合为 `currentBaseModule` 时，
对当前 BuildModel 发送一次原生 `OnUpdateInventory(Container)`。传入任一实际注入且仍可解析的普通箱子，
让原版重新计算首次建筑资格。该通知每个建造菜单会话最多一次。

### 高频查询

以下高频 Hook 只执行固定大小的会话 `touch`，不得解析 UObject 图、发现目录或修改数组：

- `PalUIBuildModel:GetBuildObjectDataArrayForUIDisplay`
- `PalBuilderComponent:IsExistsMaterialForBuildObject`

它们不承担首次联合建立职责；若必要的菜单边界 Hook 无法注册，则建造共享能力保持不可用。

### 关闭菜单

`PalUIInGameMainMenuBuildModel:Dispose` post-hook 是建造菜单的正常关闭入口。它：

1. 只释放当前世界代次的建造会话；
2. 按账本恢复当前据点模块；
3. 清空会话的资格刷新标记；
4. 不执行目录发现。

退出建造模式、关闭共享、LoadMap 前置和卸载继续作为恢复兜底。

### 提交验证

`PalNetworkPlayerComponent:RequestBuild_ToServer` pre-hook 保持现有验证：

- 重新解析当前据点一次；
- 要求当前据点等于活动联合的目标据点；
- 重读并验证联合序列；
- 任一检查失败时恢复联合并拒绝继续扩展材料路径。

## 状态机调整

新增明确的建造菜单边界事件：

- `beginBuildingMenu`：只用于 `OnOpenMenu` 和 `Setup` pre-hook；
- `refreshBuilding`：只用于 `Setup` post-hook；
- `closeBuilding`：只用于 `Dispose` post-hook；
- `touch`：只用于高频只读查询。

同据点重复的 `beginBuildingMenu` 可以复用有效联合；不同据点必须先恢复旧联合再重建。
`ForegroundMaterialSession` 不保存 Unreal 指针，只保存世界代次、操作类别和资格刷新标记。

## 错误处理

- `OnOpenMenu` 获取失败时保持原版行为，`Setup` 仍可执行一次边界兜底。
- `Setup` 后资格通知失败时释放会话、恢复联合，并仅停用本世界建造共享。
- `Dispose` 恢复失败时沿用现有安全门：本世界停用制作和建造共享。
- 不允许通过重新开关功能绕过本世界恢复失败安全门。

## 测试

纯 C++ 回归测试必须覆盖：

1. Hook 清单将 `OnOpenMenu` 和 `Setup` pre 映射为建造菜单边界获取；
2. 高频列表和材料查询只映射为 `touch`；
3. `Setup` post 映射为单次资格刷新；
4. `Dispose` post 映射为建造菜单关闭；
5. 当前联合为 `currentBaseModule` 时允许 BuildModel 刷新；
6. `playerHelper` 或非建造联合不得触发建筑资格刷新；
7. 关闭建造菜单后资格刷新标记和活动会话都清空；
8. 同据点边界可复用，不同据点边界要求重建；
9. 世界代次不匹配时不得复用或释放其他世界会话。

游戏内验证必须覆盖：

- 开启共享后首次直接按 B 即可建造，不先打开炉子、不二次按 B；
- 关闭菜单并传送到另一个据点，首次按 B 即可使用共享材料；
- 建造实际扣除材料，取消建造只返还已经扣除的材料；
- 反复打开和关闭建筑菜单后，每个会话都有匹配的联合建立与恢复日志；
- 开启共享后移动、瞄准和冲刺不出现新增的持续帧时间开销。

