# CLAUDE.md

This file provides guidance to Claude Code when working in this repository. Detailed user-facing
instructions live in `README.md`; repository-wide agent rules live in `AGENTS.md`.

## 项目概览

这是一个面向 **Palworld 1.0** 的 UE4SS C++23 mod。当前 mod 名为 `PalworldEditor`
（版本 1.6.10），提供：

- 运行时物品目录、本地化搜索、给予物品和主背包数量修改；
- 数字键当前高亮、下一次按 E 会召唤的队伍帕鲁主动/被动技能编辑、被动技能分类选择和四词条预设；
- 帕鲁属性编辑：等级、个体值、四项帕鲁之魂强化、浓缩星级、性别、13 类工作适应性永久加成与亲密度；
- 帕鲁形态修改：Alpha（头目）、Lucky（闪光）与觉醒三个独立开关，需收回后应用；
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

## 分支与协作流程

- `main` 仅用于发布；开发基线是 `develop`。
- 所有修改（功能、修复、文档、重构、脚本）都必须从 `develop` 切出独立分支，禁止直接在
  `develop` 或 `main` 上提交；分支名使用 `feat/`、`fix/`、`docs/`、`chore/`、`refactor/`、
  `style/`、`test/`、`diag/` 前缀。
- 每个修改集 = 一个分支 → 推送远端 → 先提交 PR 到 `develop`；合入 `develop` 验证通过后，
  再按发布流程合入 `main`。
- 本地 `develop` 与 `main` 只用于 `pull` 对齐，不直接提交。

## 架构

- `inc/game/pal_game.hpp`：背包、物品、当前待出战队伍帕鲁的反射访问，以及 `TryGetSpawnedOtomoHandle` 出战状态检测；
- `inc/items/item_catalog.hpp`：本地化物品标签、搜索、去重和 Raw ID 索引；
- `inc/skills/`：技能定义、目录、编辑服务、被动技能分类、显式目标锁定与世界代次状态；
- `inc/pal_stats/`：帕鲁属性编辑领域与 `SaveParameter` 反射适配，包括四项帕鲁之魂强化、浓缩星级、性别、13 类工作适应性永久加成，核心属性与工作适应性使用独立安全停用域；
- `inc/pal_identity/` + `src/pal_identity.cpp`：Alpha/Lucky/觉醒三维形态编辑；普通/`BOSS_` CharacterID 配对、`IsRarePal` 与 `bIsAwakening` 的显式游戏线程事务，只允许收回状态，失败整笔回滚；
- `inc/grappling_hook/` + `src/grapple_cooldown_gateway.cpp`：精确识别爪钩对象的一次性冷却覆盖与原值恢复；
- `src/pal_skills.cpp`：技能领域接口到 Palworld UFunction 的游戏线程适配；
- `inc/base_resource_sharing/resource_pool.hpp`：公会资源过滤、能力与按注入次数恢复的纯逻辑；
- `inc/base_resource_sharing/persistent_union.hpp`：持久登记图、差量、生命周期、边账本与帧预算纯逻辑；
- `inc/base_resource_sharing/resource_session.hpp`：旧菜单会话契约的纯逻辑回归覆盖，不再参与运行时协调；
- `inc/base_resource_sharing/hook_manifest.hpp`：Palworld 1.0.1 持久登记结构事件与提交边界 Hook 清单；
- `inc/base_resource_sharing/pal_base_resources.hpp` + `src/pal_base_resources.cpp`：世界级持久登记生命周期、结构失效合并、增量差量调度与 Hook 适配；
- `src/pal_base_resource_runtime.*`：通过 Palworld 管理器发现同公会普通仓储、调用原生 ConcreteModel 登记/注销接口并按边账本恢复；
- `src/dllmain.cpp`：mod 生命周期、ImGui、EngineTick/LoadMap 和线程间请求交接。

ImGui 回调只处理标准库值、原子请求和互斥锁快照。UObject 反射读写只允许在 EngineTick
或相应 UFunction 的游戏线程 Hook 内执行；跨帧状态不得持有 UObject 指针或 Unreal 数组地址。

当前技能目标只在用户点击“选择当前帕鲁”后锁定。数字键切换不会自动切换编辑对象；修改前仍会重新校验
当前 GUID。LoadMap 前必须清空请求并撤销写权限，进入新世界后必须重新选择。

被动技能分类选择器在新增/替换流程中提供”类别 + 技能”两级下拉框。分类来源是 `PalPassiveSkillManager:GetSkillData`
的 `Rank` 与 `AddWorldTreePal`：传说优先，其次按 `Rank` 划分负面、极品、稀有、普通。分类只在被动目录成功刷新后
由增量任务驱动，每个 EngineTick 最多读取 8 个 ID 且受 500 微秒软预算约束；单个 `GetSkillData` 失败只标记未知，
不终止任务。成功元数据缓存保留到卸载，手动刷新只重试新 ID 与失败 ID；分类完成前只有”全部”可选，结构性失败
若有旧分类则保留具体类别可用。LoadMap 前取消任务但保留缓存。1.6.5 增加该选择器，不改变主动技能目录、被动
写入、四词条预设、目标锁定与资源共享，也不引入常驻扫描或逐帧任务。

属性编辑涵盖等级（1–80）、个体值 HP/攻击/防御（0–100）、四项帕鲁之魂强化（0–20）、浓缩星级（0–运行时上限）、
性别（雄性/雌性）与亲密度（0–10）。帕鲁之魂直接写入 Rank 字段并调用 `OnRep_SaveParameter` 刷新；浓缩同步
`Rank`/`RankUpExp`，伙伴技能等级由 Rank 派生只读显示。工作适应性面板直接编辑 `GotWorkSuitabilityAddRankList`
中的绝对永久附加值，合计等级由原生基础读数与附加值相加显示，提交时以相对当前值的有符号差值调用
`SetWorkSuitabilityAddRank` 增量写入；物种原本不具备的方向不能新增。工作适应性拥有独立安全停用域，不会因
验证失败而禁用基础属性。Alpha 通过原生数据库确认的普通/`BOSS_` CharacterID 配对切换，Lucky 写入 `IsRarePal`，
觉醒写入 `bIsAwakening`，三者可任意组合且不消耗材料。为避免场上 Actor 与存档状态分裂，形态修改只允许在
目标帕鲁已收回时执行；出战判断以本地 Holder `TryGetSpawnedOtomoHandle` 为准，不使用可能残留的 Actor 对象。

## 资源共享契约

资源目录通过 `PalBaseCampManager:GetBaseCampIds` / `TryGetModel`、`PalBaseCampModel.ModuleArray`、
`PalBaseCampModuleItemStorage.ContainerInfos` 和 `PalMapObjectManager:FindConcreteModel` 直接建立。
只接受同公会、已加载、类型为 `Chest` 的普通仓储。不得使用全局 `FindAllOf`，不得扫描或修改
`ItemSlotArray` / `StackCount`。

共享实现是世界代次内持续存在、可逆的公会仓储持久登记图：对每个同公会目标仓储模块，使用
`OnAvailableConcreteModel_ServerInternal` 登记其他据点已加载的普通箱子；目标据点自己的原生容器不重复登记。
制作和建造都只依赖 Palworld 原生仓储关系，不再修改本地主背包 `InventoryMultiHelper`，也不再把联合绑定到
建筑或制作菜单生命周期。菜单关闭、进入放置预览、连续建造和提交扣除期间不得恢复或重建联合。

`OnAvailableConcreteModel_ServerInternal` 与 `OnNotAvailableConcreteModel_ServerInternal` 的 pre-hook
在参数仍由引擎持有时读取 ConcreteModel 身份，并只查询由上一次安全目录建立、完成登记/注销后增量维护的排序
纯值索引；不得在 Hook 内遍历模块或 `ContainerInfos`。post-hook 不解引用参数对象。
`PalBaseCampModel:OnRep_ModuleArray` 只在初始化明确等待仓储结构时唤醒，稳定状态下周期性的无变化复制必须忽略。
下一次 EngineTick 重新发现一次安全目录、剔除边账本中已注入到目标模块的容器，再计算期望边与已应用边的最小差量，
避免把本 Mod 的注入结果当成原生来源并递归扩张。新增/删除每帧最多执行 4 条，并受 500 微秒软预算限制。

每条由本 Mod 新增的边只保存目标/来源据点 GUID、容器 GUID、ConcreteModel 所有者 GUID 和目标模块对象全名。
登记前后必须验证目标容器为精确 0→1 且只在原序列尾部追加；异常立即调用原生注销接口回滚。注销必须验证精确
1→0 且其他序列不变。恢复失败造成的本世界安全禁用不能通过切换开关绕过；关闭开关仍必须进入 restoring 阶段
尽力清理剩余账本。每次安全目录发现还必须把已发现目标模块中的实物登记与边账本交叉验证。

本地权限门为 `IsServer && !IsDedicatedServer`。修理共享仍不可用。不要与 IntegratedStorage、
UBIM Lite、BlueprintResearch 或其他修改相同资源路径的 mod 同时测试。

## 验证

提交前至少运行：

```powershell
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor PalworldEditorTests PalworldEditorBaseResourceSharingTests
ctest --test-dir build --output-on-failure
git diff --check
```

游戏内应看到 `PalworldEditor loaded (v1.6.10)`。除物品、技能和世界切换回归外，还要验证：

- 关闭资源共享时，工厂和建造界面性能与未启用资源功能一致；
- 同一世界内关闭后重新开启会自动恢复非零计数，每次重新开启只产生一次成功目录校准；
- 无论是否已锁定帕鲁，空闲等待至少 10 秒都不再触发 Holder 全局解析；数字键切换后提交编辑仍会即时拒绝错误目标；
- 选择预设不会写入，点击”应用预设”才提交一次请求；相同预设零写入，部分失败时恢复原四词条；
- 开启后反复进入制作/建造会话不持续掉帧；
- 首次打开建筑菜单时图标即可选择，无需先打开炉子；
- 制作最大数量与真实可制作数量一致，不重复统计同一箱子；
- 据点 A 能预览并真实消费据点 B 已加载普通箱子的材料；
- 材料不足不扣除，退出会话、关闭开关和 LoadMap 后恢复原版行为；
- 食物箱、运输、自动生产、箱子 UI 和修理不共享；
- 每个登记边都有匹配的恢复日志，稳定状态下没有目录校准或差量重建；
- 连续冷启动多次，进入主界面前不发生反射调用崩溃；
- 四项帕鲁之魂强化均可在 0–20（含边界）内独立修改，浓缩 0 星和运行时最大星级、雄性/雌性均可独立写入并在重读后保持；
- 工作适应性只强化物种原有方向，以绝对永久附加值编辑并由独立按钮和安全域提交，合计等级只读显示；
- 形态修改在帕鲁收回时可执行，场上时禁用；Alpha、Lucky、觉醒三个维度独立且不消耗材料；
- 任一字段不可访问或写后重读不一致时对应领域整笔拒绝或回滚，基础属性、工作适应性与形态的安全停用域彼此独立。
