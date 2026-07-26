# PalworldEditor 据点资源共享开关重新激活设计

## 背景与根因

PalworldEditor 1.6.0 在同一世界内关闭“同公会跨据点资源共享”时会正确执行完整停用流程：

- 重置制作/建造资源会话；
- 重置资源目录调度器；
- 恢复活动资源联合；
- 注销资源 Hook；
- 清空目录、世界上下文名称以及据点和容器计数。

但是，从关闭状态重新打开时只更新 `RuntimeState` 中的用户偏好。资源会话和目录调度器没有按当前世界代次重新
初始化。由于 `ReconcileScheduler` 仍处于 `reset()` 后的零代次、非 pending 状态，后续 EngineTick 永远不会
启动目录发现，界面计数持续为 0，制作和建造共享也保持失效。

退出并重进世界会调用 `on_world_ready()`，因此可以暂时恢复。这证明缺口位于“同一世界内重新开启”的生命周期
分支，而不是据点发现或配置持久化。

## 目标

- 在可访问世界中从关闭切换为开启后，按当前世界代次重新初始化资源会话和目录调度器。
- 安排一次安全的初始目录校准，使据点和容器计数恢复，Hook 能力重新解析。
- 保持关闭路径的恢复、清空与注销顺序不变。
- 保持世界尚不可访问时的安全门；此时由后续 `on_world_ready()` 完成初始化。
- 不增加逐帧扫描、后台线程、全局 UObject 枚举或物品槽位读取。
- 一次重新开启只产生一次成功的初始校准，不形成新的常驻掉帧路径。
- 目标版本为 PalworldEditor 1.6.1。

## 非目标

- 不处理“材料数量已显示但建筑图标不可选”的早期资格计算问题；对应的大型实施计划继续暂停。
- 不改变据点数量当前代表“提供普通仓储资源的据点数”的语义。
- 不改变 8 秒空闲兜底校准、1 秒失败退避或现有资源联合算法。
- 不增加第五据点、空据点或其他材料操作场景的支持逻辑。
- 不部署到游戏目录，除非用户另行要求。

## 方案比较

### 方案 A：重新开启时恢复当前世界运行态

识别 `false → true` 转换。如果世界可访问，则用 `runtime_.generation()` 调用资源会话和调度器的
`begin_world()`；下一次安全 EngineTick 执行一次既有目录校准。

优点：

- 直接补齐缺失的生命周期对称性；
- 复用已经验证的世界初始化与调度逻辑；
- 不保留关闭前目录，不使用可能过期的对象名称或容器 GUID；
- 不增加新的扫描机制或周期。

缺点：

- 重新开启后的第一次校准仍会在游戏线程执行一次管理器遍历；
- 极端硬件或异常多据点下无法承诺绝对零微小帧波动。

### 方案 B：关闭时保留目录，重新开启直接复用

重新开启更快，但关闭期间据点、箱子、世界流送或公会状态可能已经变化。复用旧目录会增加错误联合和恢复失败
风险，因此不采用。

### 方案 C：把目录发现拆成每帧一个据点

可以进一步限制单次工作量，但需要引入跨帧发现状态、部分目录、取消和世界切换处理。当前问题只有一次五据点
级管理器遍历，复杂度和新风险明显高于收益，因此不采用。

## 选定设计

采用方案 A。

### 纯值转换决策

在不依赖 Unreal 的资源会话头文件中增加纯值决策：

```cpp
struct ResourceToggleTransition {
    bool disableRuntime{};
    bool beginAccessibleWorld{};
};

[[nodiscard]] constexpr auto decide_resource_toggle(
    bool wasEnabled, bool requestedEnabled, bool worldAccessible) noexcept
    -> ResourceToggleTransition;
```

语义：

- 状态未变化：两个字段均为 `false`；
- `true → false`：`disableRuntime == true`；
- `false → true` 且世界可访问：`beginAccessibleWorld == true`；
- `false → true` 且世界不可访问：不立即初始化，等待 `on_world_ready()`。

该函数只表达转换决策，不保存状态、不执行反射，可由纯 C++ 测试覆盖。

### 桥接生命周期

`PalBaseResourceBridge::Impl::set_enabled()` 先读取旧启用状态并计算转换：

1. `disableRuntime` 时沿用现有完整停用流程。
2. 更新 `runtime_.set_preference(enabled)`。
3. `beginAccessibleWorld` 时：
   - 读取当前 `runtime_.generation()`；
   - 调用 `leases_.begin_world(generation)`；
   - 调用 `scheduler_.begin_world(generation)`；
   - 清除普通运行时错误；
   - 不清除因联合恢复失败而设置的本世界安全禁用错误。
4. 发布快照。

`dllmain.cpp` 当前 EngineTick 顺序保持不变：

1. 消费 GUI 开关请求并调用 `set_enabled()`；
2. 注册/解析资源 Hook；
3. 调用资源桥 `tick()`。

因此重新开启后的同一个或下一个安全 EngineTick 可以执行初始校准，但不会从 ImGui 回调访问 Unreal。

## 性能约束

- 共享关闭时仍不注册资源 Hook、不调度目录发现。
- 重新开启只调用两个纯状态 `begin_world()`，不在 `set_enabled()` 内直接扫描。
- 初始校准继续使用 `GetBaseCampIds`、`TryGetModel`、`ModuleArray` 和 `ContainerInfos`。
- 不使用 `FindAllOf`，不读取 `ItemSlotArray` 或 `StackCount`。
- 不创建线程，不把 UObject 访问移出游戏线程。
- 成功校准后由既有调度器清除 pending 状态，不会每帧重复。
- 失败时沿用 1 秒退避，不在每帧重试。
- 保留现有目录校准耗时日志，用于确认开关重启只产生一次成功校准。

该修复保证不增加常驻或逐帧重型路径；单次目录校准的绝对耗时仍取决于硬件、已加载据点和箱子数量。

## 错误处理

- 世界不可访问：只记录启用偏好，不初始化；`on_world_ready()` 负责后续启动。
- 必需 Hook 尚未解析：目录可以校准，但对应制作/建造能力继续保持不可用，等待既有 Hook 重试。
- 初始目录校准失败：显示现有错误并按 1 秒退避重试。
- 联合恢复曾失败并触发本世界安全禁用：关闭再打开不能绕过该安全门，必须切换世界。
- 重复点击当前状态：不重置调度器、不重新扫描。

## 测试

纯 C++ 测试覆盖：

- 未变化的开关状态不产生动作；
- 开启到关闭请求完整停用；
- 可访问世界中的关闭到开启请求当前世界初始化；
- 不可访问世界中的关闭到开启等待 `on_world_ready()`；
- 调度器 `reset()` 后以同一世界代次重新 `begin_world()`，下一次 `advance()` 只触发一次初始校准；
- 初始校准完成后，下一帧不会再次触发；
- 错误代次仍不能触发调度器。

构建验证：

```powershell
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor PalworldEditorTests PalworldEditorBaseResourceSharingTests
ctest --test-dir build --output-on-failure
git diff --check
```

游戏内验证：

1. 进入单人世界或本地房主世界，开启共享并等待计数正常。
2. 关闭共享，确认计数清零且原版行为恢复。
3. 不退出世界，重新开启共享。
4. 确认据点和容器计数自动恢复，无须重进存档。
5. 连续重复关闭/开启三次，每次只出现一条成功目录校准日志。
6. 在工厂和建筑菜单停留 2–3 分钟，确认没有新增逐帧日志、周期性扫描或可感知掉帧。
7. 在标题界面切换偏好后进入世界，确认世界安全门之前不执行资源反射。
