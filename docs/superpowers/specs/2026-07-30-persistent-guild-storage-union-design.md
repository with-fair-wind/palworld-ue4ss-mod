# 持久公会仓储联合重构设计

**日期：** 2026-07-30  
**状态：** 已确认  
**取代：** `2026-07-30-building-menu-session-boundaries-design.md`

## 问题与根因

当前跨据点资源共享把材料联合绑定到建筑菜单模型：

- `OnOpenMenu` / `Setup` 建立联合；
- `Setup` 后调用 `BuildModel::OnUpdateInventory` 刷新；
- `PalUIInGameMainMenuBuildModel::Dispose` 恢复联合。

游戏从建筑菜单切换到放置预览时也会销毁菜单模型，因此 `Dispose` 不是可靠的“退出建造”边界。
这会在预览、连续建造和实际扣除前提前撤销共享。当前代码还直接复制 `ContainerInfos`，
该操作可以影响部分数量查询，却没有证据证明它完成了原生仓储登记所需的容器监听和 Builder 资格更新。

公开的 IntegratedStorage 3.2 使用另一条已验证路线：在服务器/本地主机上调用
`PalBaseCampModuleItemStorage::OnAvailableConcreteModel_ServerInternal`，把同公会箱子的
ConcreteModel 登记到其他同公会据点模块，使原生制作、建造资格和扣除始终读取同一套容器关系。

## 目标

- 开启共享并初始化完成后，第一次按 B 即可选择共享材料足够的建筑。
- 菜单切换到放置预览、连续建造、提交与取消期间不拆除联合。
- 传送到任意同公会据点后不需要重建“当前据点会话”。
- 制作与建造均使用 Palworld 原生仓储消费路径，避免重复统计。
- 动态关闭、LoadMap 前置和功能故障时能精确撤销本 Mod 新增的登记。
- 稳定状态不执行 UObject 扫描、定时目录遍历或高频材料 Hook 工作。
- 不引入 AOB detour、裸结构偏移、远程客户端槽位伪造或新的后台线程。

## 总体架构

资源共享改为一个世界代次内持续存在的“公会仓储图”：

```text
同公会据点仓储模块 × 同公会普通箱子 ConcreteModel
                    │
                    ▼
             期望跨据点登记边
                    │
           原生 OnAvailable 登记
                    │
                    ▼
       原生制作 / 建造 / 资格 / 扣除
```

每条边表示“目标据点仓储模块登记一个其他据点的普通箱子”。本据点自己的箱子、特殊容器、
其他公会容器和已经存在的登记不形成注入边。

## 纯值领域

新增 `persistent_union.hpp`，只包含标准库值类型：

- `PersistentUnionEdge`：目标据点、目标模块全名、来源据点、容器 ID、地图物体 ID；
- `PersistentUnionPlan`：去重且稳定排序后的期望边；
- `PersistentUnionDiff`：相对于已登记边的新增和删除集合；
- `PersistentUnionLifecycle`：`off`、`initializing`、`ready`、`reconciling`、
  `restoring`、`failed` 状态；
- `PersistentUnionWorkBudget`：每帧最大操作数和软时间预算；
- `PersistentUnionLedger`：只记录已经验证为本 Mod 新增的边。

所有比较和去重以 GUID 与对象全名完成。跨帧不保存 UObject 指针、属性地址或数组地址。

## 原生登记与恢复

### 登记

每个增量工作项在游戏线程内：

1. 通过对象全名重新解析目标仓储模块；
2. 通过地图物体管理器和地图物体 GUID 重新解析 ConcreteModel；
3. 登记前重读目标 `ContainerInfos`；
4. 已存在容器 ID 时视为原生/其他来源登记，不写入本 Mod 账本；
5. 调用 `OnAvailableConcreteModel_ServerInternal(ConcreteModel)`；
6. 重读并要求该容器恰好出现一次；
7. 只有从“不存在”变为“恰好一次”才记录为本 Mod 新增边。

### 注销

关闭、世界切换或差量删除时：

1. 只处理账本记录的边；
2. ConcreteModel 仍可解析时调用
   `OnNotAvailableConcreteModel_ServerInternal(ConcreteModel)`；
3. 重读验证本 Mod 的登记已经移除且其他顺序仍有效；
4. ConcreteModel 已卸载时，使用现有按容器 ID 精确删除算法作为后备；
5. 任何无法验证的恢复都会进入 `failed`，本世界不允许通过重新开关绕过。

## 调度与帧时间

首次开启、世界进入后的初始化和结构失效后的差量处理均由 EngineTick 驱动，但只在存在工作项时执行：

- 每帧最多处理 4 条原生登记/注销边；
- 每次 `ProcessEvent` 后检查 500 微秒软预算；
- 达到任一限制立即让出；
- 稳定 `ready` 状态只进行常量状态判断；
- 不设置 8 秒或其他周期性扫描。

目录变化由以下低频事件合并为一个失效标志：

- `PalBaseCampModuleItemStorage::OnAvailableConcreteModel_ServerInternal`；
- `PalBaseCampModuleItemStorage::OnNotAvailableConcreteModel_ServerInternal`；
- `PalBaseCampModel::OnRep_ModuleArray`。

自有登记通过 mutation guard 忽略，避免递归触发校准。失效后先发现一次当前目录并计算差量，
不会无条件拆除并重建全部边。

## Hook 调整

删除与菜单会话相关的必需 Hook：

- `PalUIBuildModel::GetBuildObjectDataArrayForUIDisplay`；
- `PalBuilderComponent::IsExistsMaterialForBuildObject`；
- `PalUIInGameMainMenuBuildModel::Setup`；
- `PalUIInGameMainMenuBuildModel::Dispose`；
- `PalBuilderComponent::ChangeMode`。

保留：

- 结构变化 Hook：只标记目录失效；
- `PalUIBuildModel::OnOpenMenu` 和 `PalUIConvertItemModel::Initialize`：仅作为低频就绪检查，
  不建立或恢复联合；
- `RequestBuild_ToServer` 与 `StartProduction`：提交前只验证持久联合仍处于当前世界且账本可读，
  不扫描目录；
- LoadMap、动态开关和卸载安全门。

建筑菜单的创建、销毁和放置预览不再改变共享关系。

## 初始化状态与界面

开启开关后状态依次为：

```text
初始化目录 → 增量登记 → 已就绪
```

初始化期间保持原版材料行为，并显示进度。共享只有在全部计划边验证完成后才报告“制作/建造可用”。
关闭时显示“正在恢复”，全部账本边撤销后才进入关闭状态。

若开启时没有任何远端普通箱子，空计划可以直接进入 `ready`，原生行为不变。

## 最小诊断验证

完整启用持久联合前，运行时实现必须支持单边验证：

1. 选取一个目标模块和一个远端普通箱子；
2. 原生登记并重读；
3. 记录登记成功；
4. 原生注销并重读；
5. 两次验证均成功后才允许执行剩余计划。

若原生登记后第一次按 B 仍灰显，下一项独立实验才允许尝试一次
`PalBuilderComponent::OnUpdatePossessMaterials` 广播。该广播不是默认方案，也不和其他猜测性刷新同时加入。

## 测试

纯 C++ 测试覆盖：

- 期望边排除同据点、自有和重复容器；
- 边集合稳定排序；
- 新增/删除差量；
- 状态机的开启、就绪、失效、恢复、失败和世界代次隔离；
- 每帧操作数预算；
- 账本只记录真正新增的边；
- 恢复保留非注入运行时条目；
- Hook 清单不再包含高频资格查询和菜单 `Dispose`；
- 结构事件只产生失效标记；
- 菜单打开和提交不会释放联合。

游戏内验证覆盖：

- 首次按 B、放置预览、连续建造和实际扣除；
- 不打开炉子也能立即建造；
- 传送后首次按 B；
- 制作数量和实际扣除；
- 新建、拆除普通箱子；
- 动态关闭和重新开启；
- 退出世界、重新进入存档；
- 稳定等待和操作时的帧时间采样。

## 非目标

- 不支持远程客户端或专用服务器；
- 不注入伪造物品槽位；
- 不修改 `ItemStackInfo`；
- 不共享食物箱、生产输入、运输和自动生产容器；
- 不在本次重构中启用修理材料共享。
