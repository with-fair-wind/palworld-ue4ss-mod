# 复活队伍帕鲁 Design

**日期：** 2026-08-07
**目标：** 帕鲁 Tab 加「复活队伍帕鲁」按钮，一键复活所有死亡队伍帕鲁。安全、不掉帧。

## 背景

- 帕鲁死亡后处于 `Dying`/`DeadBody`/`CloudCemetery` 状态。
- `PalIndividualCharacterParameter::SetPhysicalHealth(EPalStatusPhysicalHealthType)` 可直接设为 `Healthful`（=0）复活。
- `PalIndividualCharacterParameter::IsDead()` 判断是否死亡。
- `PalOtomoHolderComponentBase::GetMaxOtomoNum()` 返回队伍最大槽位数。
- `PalOtomoHolderComponentBase::GetOtomoIndividualHandle(int32 SlotIndex)` 按槽位获取 Handle。

## 设计

### UI
- 帕鲁 Tab（`render_pal_editor`）加「复活队伍帕鲁」按钮，与「选择当前帕鲁」同级。
- 按钮点击 → `wantReviveTeam_.store(true)`（原子请求）。
- 结果消息通过 `skillRuntimeSnapshot_.lastResult` 发布（复用现有技能快照发布通道，不引入新 UI 状态）。

### 复活流程（`game_thread_tick` 消费 `wantReviveTeam_`）

```
1. FindFirstOf("PalOtomoHolderComponentBase") → Holder
2. is_valid(Holder) → 否则跳过
3. invoke<int>(Holder, "GetMaxOtomoNum") → maxNum
4. for (slotIndex = 0; slotIndex < maxNum; slotIndex++):
   a. invoke<UObject*>(Holder, "GetOtomoIndividualHandle", slotIndex) → Handle
      （注意：GetOtomoIndividualHandle 有入参 slotIndex，不能用 invoke<T>）
   b. is_valid(Handle) → 否则 continue
   c. invoke<UObject*>(Handle, "TryGetIndividualParameter") → Parameter
   d. is_valid(Parameter) → 否则 continue
   e. invoke<bool>(Parameter, "IsDead") → isDead
   f. if (isDead):
      - ProcessEvent SetPhysicalHealth(PhysicalHealth=Healthful=0)
      - revivedCount++
5. skillRuntimeSnapshot_.lastResult = "复活了 N 只队伍帕鲁。"
6. skillSnapshotDirty_ = true; publish_skill_snapshot_if_dirty()
```

### SetPhysicalHealth 调用细节（有入参，不能用 invoke<T>）

```cpp
auto* fn = parameter->GetFunctionByNameInChain(STR("SetPhysicalHealth"));
pal_game::FunctionParams params{fn};
auto* prop = fn->FindProperty(FName(STR("PhysicalHealth"), FNAME_Find));
// 设为 Healthful = 0（EPalStatusPhysicalHealthType 枚举的首个值）
if (auto* enumProp = CastField<FEnumProperty>(prop)) {
    enumProp->GetUnderlyingProperty()->SetIntPropertyValue(
        enumProp->ContainerPtrToValuePtr<void>(params.data()), 0);
}
parameter->ProcessEvent(fn, params.data());
```

### GetOtomoIndividualHandle 调用细节（有入参 slotIndex）

```cpp
auto* fn = holder->GetFunctionByNameInChain(STR("GetOtomoIndividualHandle"));
pal_game::FunctionParams params{fn};
auto* slotProp = fn->FindProperty(FName(STR("SlotIndex"), FNAME_Find));
// 设 slotIndex
if (auto* intProp = CastField<FIntProperty>(slotProp)) {
    intProp->SetPropertyValueInContainer(params.data(), slotIndex);
}
holder->ProcessEvent(fn, params.data());
auto* retProp = CastField<FObjectPropertyBase>(
    fn->FindProperty(FName(STR("ReturnValue"), FNAME_Find)));
auto* handle = retProp != nullptr
    ? retProp->GetObjectPropertyValue(retProp->ContainerPtrToValuePtr<void>(params.data()))
    : nullptr;
```

### 安全措施（不崩溃）

1. **游戏线程**：所有 UFunction 调用在 `game_thread_tick`（EngineTick 回调），不在 GUI 线程。
2. **is_valid 检查**：Holder、Handle、Parameter 每步检查；null → continue（跳过单只，不影响其他）。
3. **不缓存 UObject**：一次性操作，遍历中不存 UObject 指针到跨帧状态。
4. **原子请求**：GUI 线程只 `wantReviveTeam_.store(true)`，游戏线程 `exchange(false)` 消费（和现有 `want_read_`/`want_scan_items_` 同模式）。
5. **FunctionParams RAII**：有入参的 UFunction 用 `pal_game::FunctionParams`（自动 InitializeStruct/DestroyStruct）。
6. **不改变技能/属性/形态**：纯复活（SetPhysicalHealth），不影响其它编辑域。

### 性能（不掉帧）

1. **一次性操作**：按钮点击 → 一次 EngineTick 消费 → 完毕。不是每帧持续监控。
2. **遍历量极小**：队伍通常 5 只帕鲁 → 5 × ProcessEvent（微秒级）。
3. **无循环/重试**：不像 scan_all_items 有 scheduler 重试。复活一次就完成。
4. **复用现有发布**：结果消息通过 `skillSnapshotDirty_` → `publish_skill_snapshot_if_dirty()`，不引入新帧开销。

### 复活范围

- **当前队伍帕鲁**（OtomoHolder 槽位 0..GetMaxOtomoNum-1）。
- **不含 PalBox**（后续扩展）。

## 验证

- 构建：`format-check` + `PalworldEditor` + `PalworldEditorTests` + `PalworldEditorBaseResourceSharingTests` + `ctest` 全绿。
- 游戏内冒烟：
  - 让队伍帕鲁死亡 → 点「复活队伍帕鲁」→ 确认复活（不再濒死/死亡）。
  - 队伍无死亡帕鲁 → 点按钮 → 显示「没有需要复活的帕鲁」。
  - 复活后帕鲁属性/技能/形态不变（SetPhysicalHealth 只改健康状态）。

## 非目标

- PalBox 复活（后续扩展）。
- HP 数值修改（SetPhysicalHealth 内部恢复 HP）。
- 复活后自动出战（不影响出战状态）。
