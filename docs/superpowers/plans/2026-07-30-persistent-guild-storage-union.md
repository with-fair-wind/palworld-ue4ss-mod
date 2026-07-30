# 持久公会仓储联合实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 用世界级、可逆的原生仓储登记替换建筑菜单临时联合，使首次建造、放置预览、传送和实际扣除始终使用一致的共享材料。

**Architecture:** 现有目录发现继续通过 Palworld 管理器安全反射完成；纯值层生成跨据点登记边与差量，
运行时在游戏线程按预算调用 `OnAvailableConcreteModel_ServerInternal` /
`OnNotAvailableConcreteModel_ServerInternal`，并用多模块账本验证和恢复。稳定状态不扫描、不轮询。

**Tech Stack:** C++23、UE4SS C++ API、CMake、Ninja、现有无 Unreal 纯 C++ 测试程序。

## Global Constraints

- 仅支持 `IsServer && !IsDedicatedServer` 的单人/本地主机。
- 动态开关每次进程启动默认关闭，不新增配置文件。
- UObject 解析、反射和 `ProcessEvent` 只在游戏线程。
- 跨帧只保存 GUID、对象全名和标准库值。
- 每帧最多处理 4 条边，500 微秒软预算。
- 不新增 AOB、裸偏移、`FindAllOf`、周期扫描或后台线程。
- 恢复失败后本世界安全禁用，重新开关不能绕过。

---

### Task 1: 持久登记图与差量

**Files:**
- Create: `mods/PalworldEditor/inc/base_resource_sharing/persistent_union.hpp`
- Modify: `mods/PalworldEditor/tests/base_resource_sharing_tests.cpp`

**Interfaces:**
- Produces: `PersistentUnionEdge`、`PersistentUnionPlan`、`PersistentUnionDiff`、
  `make_persistent_union_plan()`、`diff_persistent_union()`

- [ ] 写失败测试：跨据点全连接排除本据点、重复容器和无效 GUID。
- [ ] 运行 `PalworldEditorBaseResourceSharingTests`，确认因接口不存在而失败。
- [ ] 实现稳定排序的期望边和线性/集合差量。
- [ ] 重新运行测试并确认通过。

### Task 2: 生命周期、账本与预算

**Files:**
- Modify: `mods/PalworldEditor/inc/base_resource_sharing/persistent_union.hpp`
- Modify: `mods/PalworldEditor/tests/base_resource_sharing_tests.cpp`

**Interfaces:**
- Produces: `PersistentUnionPhase`、`PersistentUnionLifecycle`、
  `PersistentUnionLedger`、`PersistentUnionWorkBudget`

- [ ] 写失败测试：开启、初始化完成、失效合并、差量完成、恢复、失败和世界代次隔离。
- [ ] 写失败测试：账本幂等记录与每帧最多 4 项预算。
- [ ] 运行测试并确认预期失败。
- [ ] 实现最小状态机、账本与预算。
- [ ] 运行全部纯 C++ 测试。

### Task 3: Hook 清单改为持久联合边界

**Files:**
- Modify: `mods/PalworldEditor/inc/base_resource_sharing/hook_manifest.hpp`
- Modify: `mods/PalworldEditor/tests/base_resource_sharing_tests.cpp`

**Interfaces:**
- Produces: `HookEvent::structureChanged`、`HookEvent::ensurePersistentUnion`、
  `HookEvent::validatePersistentUnion`

- [ ] 写失败测试：清单不含高频资格查询、`Setup`、`Dispose` 和 `ChangeMode`。
- [ ] 写失败测试：结构 Hook 只失效；菜单打开只检查就绪；提交只验证。
- [ ] 运行测试并确认旧清单导致失败。
- [ ] 精简清单和能力计算。
- [ ] 运行测试并确认通过。

### Task 4: 原生登记与注销适配

**Files:**
- Modify: `mods/PalworldEditor/src/pal_base_resource_runtime.hpp`
- Modify: `mods/PalworldEditor/src/pal_base_resource_runtime.cpp`
- Modify: `mods/PalworldEditor/inc/base_resource_sharing/resource_pool.hpp`
- Modify: `mods/PalworldEditor/tests/base_resource_sharing_tests.cpp`

**Interfaces:**
- Consumes: `PersistentUnionEdge`
- Produces: `apply_persistent_edge()`、`remove_persistent_edge()`、
  `validate_persistent_ledger()`

- [ ] 写失败的纯值序列测试：登记前已存在不记账；新增后恰好一次才记账；恢复保留非注入条目。
- [ ] 运行测试并确认失败。
- [ ] 实现目标模块和 ConcreteModel 的当帧重新解析。
- [ ] 通过反射调用原生 Available/NotAvailable 接口并重读验证。
- [ ] 为已卸载 ConcreteModel 保留精确数组删除后备。
- [ ] 运行全部测试。

### Task 5: 增量持久联合调度

**Files:**
- Modify: `mods/PalworldEditor/src/pal_base_resources.cpp`
- Modify: `mods/PalworldEditor/inc/base_resource_sharing/pal_base_resources.hpp`
- Modify: `mods/PalworldEditor/inc/base_resource_sharing/resource_session.hpp`
- Modify: `mods/PalworldEditor/tests/base_resource_sharing_tests.cpp`

**Interfaces:**
- Consumes: 目录快照、持久登记计划、差量、运行时边操作
- Produces: 开启初始化、结构失效差量、关闭恢复和 LoadMap 恢复

- [ ] 写失败测试：菜单关闭不改变联合；结构事件只合并失效；稳定状态没有工作。
- [ ] 运行测试并确认失败。
- [ ] 删除 `ForegroundMaterialSession` 建造菜单所有权。
- [ ] 实现初始化/差量/恢复工作队列和 4 项、500 微秒预算。
- [ ] 保证 self-mutation Hook 不触发递归失效。
- [ ] 运行全部测试。

### Task 6: 界面、文档和回归验证

**Files:**
- Modify: `mods/PalworldEditor/src/dllmain.cpp`
- Modify: `README.md`
- Modify: `AGENTS.md`
- Modify: `mods/PalworldEditor/tests/base_resource_sharing_tests.cpp`

**Interfaces:**
- Produces: 初始化/恢复/失败状态文本和最终验证说明

- [ ] 更新 GUI 快照，区分“初始化中”“已就绪”“正在恢复”“本世界失败”。
- [ ] 删除旧的建筑菜单会话和 8 秒校准描述。
- [ ] 执行格式检查、两个构建目标和 CTest。
- [ ] 执行 `git diff --check` 并检查无意改动。
- [ ] 记录必须由游戏内完成的单边原生登记验证和完整场景清单。
