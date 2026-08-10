# 物品堆叠无上限 Design

**日期：** 2026-08-07
**目标：** 物品 Tab 加开关，只把原始 `MaxStackCount == 9999` 的普通可堆叠物品提升到
999,999,999，关闭、切图和游戏线程卸载时按对象恢复原值。

## 背景

- `PalStaticItemDataBase::MaxStackCount`（int32）定义了每种物品的最大堆叠数；上限为 1 或其他特殊值
  的装备、饰品和关键物品不是候选目标。
- 游戏运行时从 .pak 文件加载到内存；mod 修改内存副本，重启游戏自然恢复。
- OFF 恢复 = 和重启游戏等价，已存在的超限堆叠不受影响（游戏不主动检查）。
- Nexus Mods 的 Max Stack Count 1.3 只修改原始上限为 9999 的对象。本实现只参考这一公开行为，使用
  独立的 C++ 事务、生命周期和测试设计，不复用其 Lua 源码。

## 设计

### 纯值领域层（`items/stack_limit_service.hpp`）

- `is_expandable_limit()` 只接受精确原值 9999；目标值为 999,999,999，仍处于 int32 安全域。
- `StackLimitOverrideLedger` 是期望状态、世界代次、待执行工作、安全停用和恢复责任的唯一所有者。
- 跨帧记录只保存 UObject 完整名称、物品 Raw ID 与原始上限，不保存 Unreal 指针。

### 游戏线程适配层（`items/stack_limit_gateway.cpp`）

- 显式请求时调用一次 `FindAllOf("PalStaticItemDataBase")`，并用实际运行时 `UClass::IsA`、
  `FNameProperty ID` 与 `FIntProperty MaxStackCount` 完整预检；主背包安全门未就绪时不消费请求。
- 先收集全部候选与原值，再使用 Property API 差量写入，并逐项重读验证。
- `PalStaticItemDataBase` 在项目现有 Palworld 1.0 能力中没有已验证的 setter/OnRep 路径；因此只对
  静态配置字段使用 Property API，并把同步重读作为可观察验证，不猜测或手写反射函数调用。
- 任一写入失败立即只回滚本事务已改对象；回滚失败时保留未恢复账本并安全停用再次应用。
- 关闭与 LoadMap 前按对象全名重新解析；仅当当前值仍是本 mod 目标值时恢复，避免覆盖其他 mod 的
  后续修改。对象暂时不可解析、身份/字段不匹配或重读失败时只保留对应记录；LoadMap 后只允许一次
  事件驱动的恢复重试，不逐帧扫描，也不把未解析对象误报为恢复成功。

### 编排（`game_thread_tick` / 生命周期）

- `requested_stack_unlimited_` 与 `stack_setting_dirty_` 只负责 GUI → 游戏线程请求交接。
- EngineTick 只做常量时间阶段判断；只有显式开关请求会启动一次扫描/事务。
- `begin_world_transition` 必须先恢复再撤销期望；新世界不会自动重新开启。
- 卸载只有在 UE4SS 确认当前是游戏线程时才访问 Unreal；非游戏线程热卸载不承诺同步恢复，只记录
  诊断，进程退出或对象重载会丢弃内存修改。

### 前端（item_ui.cpp）

- 物品 Tab 顶部加 Checkbox「普通物品高堆叠上限（999,999,999）」。
- UI 显示等待应用、已生效、等待手动重试、恢复和安全停用阶段，以及最近一次事务诊断。

### 安全

- 不改变原始特殊上限物品，不把装备或饰品变成可堆叠物。
- 不以 `ProcessEvent` 固定参数布局调用任何函数；属性通过精确 Property 类型读写。
- 预检失败不开始写入；写后验证失败必须回滚，回滚失败必须保留恢复责任并安全停用。

### 性能

- 一次显式请求只扫描一次已加载 `PalStaticItemDataBase`；没有未经测量的帧时间承诺。
- 未找到候选后等待用户关闭并重新开启，不进行定时或逐帧重试。

## 非目标

- 不修改单个 StackCount。
- 不持久化（重启恢复原值）。
- 不 Hook AddItem。
- 不支持多人客户端绕过服务器物品规则；仅承诺本地/房主当前进程的静态数据行为。
