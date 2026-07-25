# PalworldEditor 游戏线程与跨世界技能安全设计

## 背景

PalworldEditor 当前在 `CppUserModBase::on_update()` 中扫描 `UObject`、调用
`ProcessEvent` 并执行技能修改。当前 RE-UE4SS 实现从独立的
`UE4SS-UpdateThread` 调用 `on_update()`，而不是从 Unreal 游戏线程调用。
因此这些操作可能与 `LoadMap`、垃圾回收以及 Pawn/Controller 重建并发。

现有崩溃发生在退出世界后重新加载存档期间，调用栈位于
`UEngine::LoadMap` 和 `EngineTick`，两次崩溃具有相同的访问地址、调用栈哈希
和代码偏移。运行日志还显示，本地 Holder 解析会在几十毫秒内反复从
`holderOwnerControllerUnavailable` 恢复到成功。这些证据与跨线程访问
Unreal 对象造成的生命周期竞争一致。

本设计同时处理两个安全边界：

1. 所有 Unreal 访问必须在游戏线程完成，并在世界切换时停止技能操作。
2. 主动技能目录不再展示明确属于 Human、GYM、Raid、Boss 或内部用途的条目。

## 目标

- 所有 `UObjectGlobals`、`UObject`、`UFunction` 和 `ProcessEvent` 操作只从
  UE4SS `EngineTick` 游戏线程回调执行。
- GUI 和 UE4SS UpdateThread 只提交标准库值类型请求、读取互斥量保护的快照。
- `LoadMap` 开始时使旧世界请求失效，停止写入并清空运行时技能快照。
- 世界加载完成后保留原选择的 Pal GUID 作为界面提示，但写入保持禁用。
- 用户必须再次点击“选择当前帕鲁”才能在新世界代数中恢复写入。
- 过滤高置信度的内部主动技能，同时保留普通技能和物种 Unique 技能。
- Hook 注册和注销具有明确生命周期，不留下指向已销毁 mod 对象的回调。

## 非目标

- 不实现完整的逐物种 learnset 或 `MasteredWaza` 编辑。
- 不保证所有跨物种 Unique 技能在游戏规则上合法。
- 不修改存档协议、直接写入技能数组或 DataTable。
- 不通过全局 `ProcessEvent` Hook 推断当前帕鲁。
- 不更改“只有点击选择按钮才切换编辑目标”的交互原则。

## 方案比较

### 方案 A：EngineTick 回调加 LoadMap 门控

在 Unreal 初始化后注册 `EngineTick` 和 `LoadMap` 回调。原 `on_update()`
中的 Unreal 工作迁移到 EngineTick；LoadMap 回调维护世界代数和可写状态。

优点：

- 与 UE4SS 提供的游戏线程入口一致。
- 不依赖 Lua 环境。
- 不需要高频全局 ProcessEvent Hook。
- 世界切换边界清晰，便于让旧请求失效。

缺点：

- 需要保存并注销多个 UE4SS Hook ID。
- 必须把线程间共享状态明确分层。

### 方案 B：通过 Lua ExecuteInGameThread 投递

由 `on_update()` 将 C++ 请求转交 Lua 的游戏线程执行队列。

优点：

- 可复用 Lua 已有的游戏线程调度能力。

缺点：

- C++ mod 与 Lua 实例及其生命周期耦合。
- Lua 重载、错误传播和卸载顺序更复杂。
- 对当前纯 C++ mod 没有必要。

### 方案 C：利用 ProcessEvent Hook

注册全局 ProcessEvent Hook，在游戏线程调用经过时消费请求。

优点：

- 回调位于游戏线程。

缺点：

- 调用频率高。
- 容易产生重入和递归 ProcessEvent。
- 扩大 Hook 面，与本次崩溃规避目标相冲突。

采用方案 A。

## 架构

### Hook 生命周期

`PalworldEditorMod::on_unreal_init()` 只注册回调并提交初始扫描请求，不读取
Palworld UObject。

注册以下回调：

- `RegisterEngineTickPreCallback`：调用 `game_thread_tick()`。
- `RegisterLoadMapPreCallback`：调用 `begin_world_transition()`。
- `RegisterLoadMapPostCallback`：调用 `finish_world_transition()`。

每个注册调用返回 `GlobalCallbackId`。mod 析构时逐一调用
`UnregisterCallback`。无效 ID 不执行注销。

选择 EngineTick pre-hook，使本 mod 的当帧工作发生在引擎 Tick 主体之前；
LoadMap 执行期间不会有另一个本 mod EngineTick 回调重入。

### 线程职责

GUI 线程和 UE4SS UpdateThread：

- 更新输入框、下拉框和按钮状态。
- 把请求参数复制进标准库值类型队列。
- 读取互斥量保护的物品、背包和技能快照。
- 不访问任何 Unreal 类型或对象。

游戏线程：

- 消费给予物品、背包读取/修改、物品扫描、选择当前帕鲁、技能目录刷新和
  技能修改请求。
- 解析本地队伍 Holder、当前槽位、IndividualHandle 和 IndividualParameter。
- 调用全部 Palworld UFunction。
- 发布不含 UObject 指针的结果快照。

### 世界会话状态

增加一个不依赖 Unreal 的世界会话状态组件，维护：

- 单调递增的 `worldGeneration`。
- 当前是否处于世界切换中。
- 当前世界代数是否已经由用户重新确认目标。

`begin_world_transition()`：

1. 增加世界代数。
2. 标记世界不可访问。
3. 清空技能编辑 FIFO。
4. 清除技能状态和“可写”确认。
5. 保留已选择 Pal 的 GUID 和名称，仅供界面展示。
6. 将最后结果设置为“世界已切换，请重新选择当前帕鲁”。

`finish_world_transition()`：

1. 标记 LoadMap 调用已经返回。
2. 允许后续 EngineTick 尝试解析运行时对象。
3. 不自动恢复技能写入确认。
4. 请求重新加载物品和技能目录。

用户在新世界中点击“选择当前帕鲁”且解析成功后：

1. 用当前 GUID 替换或再次确认所选目标。
2. 把目标确认绑定到当前世界代数。
3. 重读技能状态。
4. 只有此时才允许消费后续技能修改请求。

每个技能修改请求保存提交时的目标代数和世界代数。消费请求时任一代数
不匹配都拒绝执行。

### Holder 解析

本次保留现有 Holder 解析链，但只在 EngineTick 游戏线程运行。解析失败时：

- 不改变已锁定的 GUID。
- 不消费技能修改请求。
- 发布具体解析阶段作为诊断。

若迁移游戏线程后仍存在频繁的状态 3/0 抖动，再单独采集
`TryGetOwnerControlledPawn` 和 `GetOwner` 的运行时证据，避免在本次修复中
同时更换解析算法而失去归因能力。

## 主动技能目录安全

生成的 `EPalWazaID` 表继续作为数值与 Raw ID 的稳定来源，但构建 UI 目录时
排除高置信度的内部条目。

过滤规则：

- Raw ID 以 `Human_` 开头。
- Raw ID 包含 `_GYM_`。
- Raw ID 包含 `Raid`。
- Raw ID 包含 `Boss`。
- 其他后续通过测试明确标记为内部用途的精确 ID。

保留：

- 普通元素技能。
- `SelfDestruct` 等确实由正常帕鲁使用的技能。
- 不含上述内部标记的物种 `Unique_` 技能。

过滤函数是纯 C++ 函数，并由单元测试覆盖。修改服务仍执行最多三个槽位、
去重、重读验证和失败回滚。

这是最低风险过滤，不声称实现完整逐物种合法性。严格合法性需要独立引入
Palworld 1.0 learnset 数据并设计高级模式，不属于本次变更。

## 错误处理

- EngineTick Hook 注册失败时，界面显示“游戏线程回调不可用”，所有 Unreal
  功能保持禁用。
- LoadMap Hook 注册失败时，技能编辑保持禁用，避免在无法识别世界边界时写入。
- 世界切换期间到达的技能请求立即拒绝或在开始切换时清空，不延迟到新世界执行。
- 解析暂时失败只暂停操作，不自动切换目标。
- Hook 注销失败只记录警告；析构流程继续完成。
- 不在持有 GUI/快照互斥量时调用 Unreal 或用户回调。

## 测试策略

纯 C++ 测试新增以下覆盖：

- 开始世界切换会增加世界代数并关闭 Unreal 访问。
- LoadMap 返回后允许重新解析，但未经用户确认仍禁止技能写入。
- 世界切换会使旧世界代数的编辑请求失效。
- 原 Pal GUID 可用于界面提示，但不能直接恢复写权限。
- 在新世界再次确认目标后恢复写权限。
- `Human_`、`_GYM_`、`Raid` 和 `Boss` 条目被主动技能目录过滤。
- 普通技能、`SelfDestruct` 和正常 `Unique_` 技能继续保留。

构建验证：

```powershell
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor PalworldEditorTests
ctest --test-dir build --output-on-failure
git diff --check
```

游戏内验证：

1. 启动后等待技能目录自动加载。
2. 选择队伍帕鲁并执行主动、被动技能修改。
3. 退出到标题画面后重新进入同一存档。
4. 确认没有自动执行旧请求，且界面要求重新选择当前帕鲁。
5. 重新选择后确认读写恢复。
6. 连续执行多次退出/重进，观察 UE4SS 日志不再出现高速状态 3/0 抖动，
   游戏不在 LoadMap 阶段崩溃。

纯 C++ 测试无法证明 Palworld 反射和切图生命周期正确，最终仍需上述游戏内
端到端验证。

