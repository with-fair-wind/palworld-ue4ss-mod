# CLAUDE.md

This file provides guidance to Claude Code when working in this repository. Detailed user-facing
instructions live in `README.md`; repository-wide agent rules live in `AGENTS.md`.

## 项目概览

这是一个面向 **Palworld 1.0** 的 UE4SS C++23 mod。当前 mod 名为 `PalworldEditor`
（版本 1.6.5），提供：

- 运行时物品目录、本地化搜索、给予物品和主背包数量修改；
- 数字键当前高亮、下一次按 E 会召唤的队伍帕鲁主动/被动技能编辑、被动技能分类选择和四词条预设；
- 默认关闭、仅支持单人/本地房主的同公会跨据点制作与建造材料共享。

Palworld 1.0 需要 UE4SS Experimental (Palworld) + PalSchema（含
`MemberVariableLayout.ini`）。F10 游戏控制台不可用，所有用户交互都通过 UE4SS GUI 的
`PalworldEditor` 页签完成。

## 构建

所有 CMake 命令都必须在 Visual Studio x64 开发者环境中运行。还需要 CMake ≥ 3.22、
Ninja、Git 和 Rust stable。

```powershell
pwsh scripts/setup.ps1
$env:PALWORLD_INSTALL_DIR = "F:\...\Palworld"  # 可选；必须在配置前设置
cmake --preset ninja-msvc-x64
cmake --build --preset ninja-msvc-x64 --target PalworldEditor
cmake --build --preset ninja-msvc-x64 --target PalworldEditorTests PalworldEditorBaseResourceSharingTests
ctest --test-dir build --output-on-failure
cmake --build --preset ninja-msvc-x64 --target deploy
```

DLL 输出为 `build/Game__Shipping__Win64/bin/PalworldEditor.dll`；部署目标为
`Pal/Binaries/Win64/ue4ss/Mods/PalworldEditor/dlls/main.dll`。

## 架构

- `inc/game/pal_game.hpp`：背包、物品和当前待出战队伍帕鲁的反射访问；
- `inc/items/item_catalog.hpp`：本地化物品标签、搜索、去重和 Raw ID 索引；
- `inc/skills/`：技能定义、目录、编辑服务、被动技能分类、显式目标锁定与世界代次状态；
- `src/pal_skills.cpp`：技能领域接口到 Palworld UFunction 的游戏线程适配；
- `inc/base_resource_sharing/resource_pool.hpp`：资源过滤、能力和恢复纯逻辑；
- `inc/base_resource_sharing/resource_session.hpp`：目录校准调度与制作/建造会话租约；
- `inc/base_resource_sharing/hook_manifest.hpp`：结构事件和制作/建造会话 Hook 清单；
- `src/pal_base_resource_runtime.*`：通过 Palworld 管理器发现普通仓储、应用和恢复临时联合；
- `src/pal_base_resources.cpp`：事件合并、租约、Hook 和 GUI 值快照编排；
- `src/dllmain.cpp`：mod 生命周期、ImGui、EngineTick/LoadMap 和线程间请求交接。

ImGui 回调只处理标准库值、原子请求和互斥锁快照。UObject 反射读写只允许在 EngineTick
或相应 UFunction 的游戏线程 Hook 内执行；跨帧状态不得持有 UObject 指针或 Unreal 数组地址。

当前技能目标只在用户点击“选择当前帕鲁”后锁定。数字键切换不会自动切换编辑对象；修改前仍会重新校验
当前 GUID。LoadMap 前必须清空请求并撤销写权限，进入新世界后必须重新选择。

被动技能分类选择器在新增/替换流程中提供“类别 + 技能”两级下拉框。分类来源是 `PalPassiveSkillManager:GetSkillData`
的 `Rank` 与 `AddWorldTreePal`：传说优先，其次按 `Rank` 划分负面、极品、稀有、普通。分类只在被动目录成功刷新后
由增量任务驱动，每个 EngineTick 最多读取 8 个 ID 且受 500 微秒软预算约束；单个 `GetSkillData` 失败只标记未知，
不终止任务。成功元数据缓存保留到卸载，手动刷新只重试新 ID 与失败 ID；分类完成前只有“全部”可选，结构性失败
若有旧分类则保留具体类别可用。LoadMap 前取消任务但保留缓存。1.6.5 增加该选择器，不改变主动技能目录、被动
写入、四词条预设、目标锁定与资源共享，也不引入常驻扫描或逐帧任务。

## 资源共享契约

资源目录通过 `PalBaseCampManager:GetBaseCampIds` / `TryGetModel`、`PalBaseCampModel.ModuleArray`、
`PalBaseCampModuleItemStorage.ContainerInfos` 和 `PalMapObjectManager:FindConcreteModel` 直接建立。
只接受同公会、已加载、类型为 `Chest` 的普通仓储。不得使用全局 `FindAllOf`，不得扫描或修改
`ItemSlotArray` / `StackCount`。

结构事件只合并目录失效标记；活动材料会话期间不校准或重建联合，8 秒低频校准仅在空闲时兜底。
建筑菜单 `OnOpenMenu` 与制作模型 `Initialize` 的 pre-hook 在首次资格计算前建立联合。制作只扩展本地
`InventoryMultiHelper`，建造扩展据点模块和材料助手；高频 Hook 只更新固定大小状态。`StartProduction`
返回后释放制作会话，1.5 秒仅作未提交请求的空闲兜底；首次菜单早于目录初始化时只安排下一 Tick 的一次扫描，
扫描后建立当前会话，不逐帧重试；退出建造模式后按注入次数恢复。
关闭开关、LoadMap 前和卸载时都必须先恢复再注销 Hook。

同一可访问世界内关闭后重新开启共享时，必须以当前世界代次重新初始化会话和目录调度器，由后续 EngineTick
自动校准。该路径不得增加线程、全局扫描、槽位扫描或逐帧任务；恢复失败造成的本世界安全禁用不能用开关绕过。
1.6.1 修复该开关生命周期；1.6.2 移除确认帕鲁后的空闲后台解析；1.6.3 增加首次资格计算前的资源联合、
制作唯一 Helper 入口和活动会话校准抑制；1.6.4 增加被动技能四词条预设的单请求差量应用与失败回滚；
1.6.5 增加被动技能分类选择器，以有界小批次在 EngineTick 后台分类，不增加常驻工作。

本地权限门为 `IsServer && !IsDedicatedServer`。修理共享仍不可用。不要与 IntegratedStorage、
UBIM Lite、BlueprintResearch 或其他修改相同资源路径的 mod 同时测试。

## 验证

提交前至少运行：

```powershell
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor PalworldEditorTests PalworldEditorBaseResourceSharingTests
ctest --test-dir build --output-on-failure
git diff --check
```

游戏内应看到 `PalworldEditor loaded (v1.6.5)`。除物品、技能和世界切换回归外，还要验证：

- 关闭资源共享时，工厂和建造界面性能与未启用资源功能一致；
- 同一世界内关闭后重新开启会自动恢复非零计数，每次重新开启只产生一次成功目录校准；
- 无论是否已锁定帕鲁，空闲等待至少 10 秒都不再触发 Holder 全局解析；数字键切换后提交编辑仍会即时拒绝错误目标；
- 选择预设不会写入，点击“应用预设”才提交一次请求；相同预设零写入，部分失败时恢复原四词条；
- 开启后反复进入制作/建造会话不持续掉帧；
- 首次打开建筑菜单时图标即可选择，无需先打开炉子；
- 制作最大数量与真实可制作数量一致，不重复统计同一箱子；
- 据点 A 能预览并真实消费据点 B 已加载普通箱子的材料；
- 材料不足不扣除，退出会话、关闭开关和 LoadMap 后恢复原版行为；
- 食物箱、运输、自动生产、箱子 UI 和修理不共享；
- 每个联合都有匹配的恢复日志，活动会话期间没有目录校准或联合重建；
- 连续冷启动多次，进入主界面前不发生反射调用崩溃。
