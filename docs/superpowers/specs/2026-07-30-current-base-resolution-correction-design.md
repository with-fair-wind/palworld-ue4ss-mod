# 据点资源共享当前据点解析修正设计

**日期：** 2026-07-30
**适用版本：** PalworldEditor 1.6.9、Palworld 1.0.1、UE4SS Experimental

## 问题与根因

资源目录能够发现同公会的 5 个据点和 22 个普通资源容器，但制作和建造联合均被安全门拒绝，GUI 显示：

```text
无法解析本地玩家控制器或 Pawn。
```

当前实现已成功通过 `PalUtility:GetLocalPalPlayerController` 获取本地控制器，随后却调用了控制器不存在的反射
函数 `GetPawn`。Engine UHT dump 中控制器公开的反射函数实际为 `Controller:K2_GetPawn`，因此 Pawn 解析必然
失败。这一失败发生在目录发现之后、资源联合建立之前。

同时，使用 Pawn 位置和 `PalBaseCampManager:GetNearestBaseCamp` 推断当前据点并不等价于游戏确认玩家位于该据点；
在据点边缘或两个据点距离接近时可能选择错误目标。

## 采用方案

每次新的制作或建造前台会话建立联合时，以及真实提交前验证时：

1. 通过 `PalUtility:GetLocalPalPlayerController` 获取本地控制器；
2. 调用反射函数 `K2_GetPawn` 获取本地 Pawn；
3. 从 Pawn 的 `InsideBaseCampCheckComponent` 对象属性取得游戏原生据点检测组件；
4. 调用 `PalInsideBaseCampCheckComponent:GetInsideBaseCampModel`；
5. 调用返回模型的公开反射函数 `GetId` 读取 GUID；
6. 验证该 ID 属于当前同公会资源目录，并且存在普通仓储模块；
7. 制作排除当前据点容器后向玩家 Helper 注入其他据点容器；
8. 建造向该当前据点仓储模块注入其他据点容器。

无法完成任一步时保持原版行为并显示具体错误，不回退到“最近据点”猜测，也不建立全据点联合。

## 生命周期与性能

- 不增加 `OnEnterBaseCamp`、`OnExitBaseCamp` 或其他事件 Hook。
- 不增加 EngineTick 工作、后台线程、定时校准或全局 UObject 扫描。
- 当前据点只在新制作/建造会话和真实提交前查询。
- 同一菜单内重复资格检查继续命中既有前台会话，不重复目录发现或数组修改。
- 提交前必须确认当前据点仍等于联合账本中的目标据点；不一致时恢复联合并安全停用对应能力。
- 只有终端、没有普通仓储模块的据点保持原版行为。

## 测试

纯 C++ 测试增加当前据点解析策略契约：

- 控制器 Pawn 函数名固定为 `K2_GetPawn`；
- 当前据点来源固定为 `InsideBaseCampCheckComponent:GetInsideBaseCampModel`；
- 不允许回退 `GetNearestBaseCamp`；
- 制作仍排除当前据点容器；
- 建造仍使用当前据点仓储模块；
- 提交前目标据点不一致时拒绝继续使用旧联合。

构建验证：

```powershell
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor PalworldEditorTests PalworldEditorBaseResourceSharingTests
ctest --test-dir build --output-on-failure
git diff --check
```

游戏内验证：

1. 进入有普通箱子的据点后开启共享，首次按 B 即可看到并使用远端材料；
2. 首次打开制作设施即可使用远端材料；
3. 建造实际扣料，取消只返还本次已扣材料；
4. 制作最大数量与实际完成数量一致；
5. 离开据点后保持原版行为并显示“当前不在据点”；
6. 开启共享后瞄准、冲刺和移动没有可重复的额外帧时间尖峰。
