# 物品堆叠无上限 Design

**日期：** 2026-08-07
**目标：** 物品 Tab 加开关，ON 时所有物品 MaxStackCount → 99999，OFF / LoadMap 时恢复原值。

## 背景

- `PalStaticItemDataBase::MaxStackCount`（int32）定义了每种物品的最大堆叠数。
- 游戏运行时从 .pak 文件加载到内存；mod 修改内存副本，重启游戏自然恢复。
- OFF 恢复 = 和重启游戏等价，已存在的超限堆叠不受影响（游戏不主动检查）。

## 设计

### 后端（game_thread_tick）

- `requestedStackUnlimited_`（atomic，GUI 设置）+ `stackSettingDirty_`（atomic，tick 消费）。
- ON（dirty + requested=true）：`ForEachUObject("PalStaticItemData")` → 读取 ID + MaxStackCount → 缓存到 `originalStackLimits_`（map<string, int32>）→ 写入 99999。
- OFF（dirty + requested=false）：`ForEachUObject("PalStaticItemData")` → 读取 ID → 从缓存查找原值 → 写回。
- `begin_world_transition`：OFF 恢复 + 清空缓存。

### 前端（item_ui.cpp）

- 物品 Tab 顶部加 Checkbox「物品堆叠无上限」。
- 变化时设 `requestedStackUnlimited_` + `stackSettingDirty_`。

### 安全

- 游戏线程执行，不缓存 UObject 指针。
- 缓存以 FName ID 字符串为键，恢复时重新 ForEachUObject 查找。
- LoadMap 恢复。

### 性能

- 一次性 ForEachUObject（数千个 PalStaticItemData，~1ms），只在开关变化时执行，不每帧。

## 非目标

- 不修改单个 StackCount。
- 不持久化（重启恢复原值）。
- 不 Hook AddItem。
