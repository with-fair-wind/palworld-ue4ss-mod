# CLAUDE.md

本文件为 Claude Code 在本仓库工作提供指引。面向最终用户的说明见 `README.md`；面向 Codex 的仓库级
指引见 `AGENTS.md`（三者保持对齐，`AGENTS.md` 保留最完整的验证清单）。

## 项目概览

这是一个面向 **Palworld 1.0** 的 **UE4SS C++23 mod** 工程（CMake / Ninja super-build）。当前 mod
名为 `PalworldEditor`（版本 1.6.10），构建产物为 `PalworldEditor.dll`。Palworld 的 F10 游戏控制台
不可用，所有用户交互都通过 UE4SS GUI 的 ImGui 浮动窗口完成。

提供功能一览：

- **物品**：运行时物品目录、本地化搜索、给予物品与主背包数量修改；
- **技能**：数字键高亮"下一次按 E 会召唤"的队伍帕鲁，主动/被动技能编辑、被动技能分类选择与四词条预设；
- **帕鲁属性**：等级、个体值、四项帕鲁之魂强化、浓缩星级、性别、13 类工作适应性永久加成与亲密度；
- **形态**：Alpha（头目）、Lucky（闪光）、觉醒三个独立开关，需收回帕鲁后应用；
- **远程终端**：按键触发的跨据点终端选择，含圈内判定与战斗中禁用（默认关闭）；
- **据点资源共享**：同公会跨据点制作/建造材料共享（默认关闭，仅单人/本地房主）。

Palworld 1.0 需要 UE4SS **Experimental (Palworld)** 运行时 + PalSchema（含
`MemberVariableLayout.ini`）。

## 环境搭建及构建与验证

### 前置依赖

- **Visual Studio 2022**（最新）+ "使用 C++ 的桌面开发"工作负载（MSVC + Ninja）；C++23 通过
  `/std:c++latest` 启用；
- **CMake ≥ 3.22** 与 **Git**（`scripts/setup.ps1` 依赖）；
- **Rust stable**（`cargo`/`rustc`）：RE-UE4SS 的 `UE4SS` target 会构建 Rust 实现的 PatternSleuth
  依赖。

### 初始化与构建

所有 CMake 命令都必须在 **VS x64 开发者环境**（"x64 Native Tools Command Prompt for VS 2022" 或
VS Developer PowerShell）中运行。

```powershell
pwsh scripts/setup.ps1                          # 首次：克隆 RE-UE4SS 并初始化子模块
$env:PALWORLD_INSTALL_DIR = "F:\...\Palworld"   # 可选；必须在首次配置前设置
cmake --preset ninja-msvc-x64                   # 配置
cmake --build --preset ninja-msvc-x64 --target PalworldEditor   # 构建
```

- 输出 DLL：`build/Game__Shipping__Win64/bin/PalworldEditor.dll`；
- 部署：`cmake --build --preset ninja-msvc-x64 --target deploy` → 游戏
  `Pal/Binaries/Win64/ue4ss/Mods/PalworldEditor/dlls/main.dll`；游戏运行时会锁定该 DLL，部署前需
  退出游戏。

### 验证

提交前至少运行：

```powershell
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor PalworldEditorTests PalworldEditorBaseResourceSharingTests PalworldEditorRemotePalboxTests
ctest --test-dir build --output-on-failure
git diff --check
```

- 三个测试 target 均为**不链接 UE4SS 的纯 C++ 测试**，覆盖物品目录、技能目录与编辑、配置、资源
  共享、远程终端等纯值逻辑；反射调用、ImGui 与 Palworld 存档效果仍需游戏内端到端验证；
- 游戏内应看到 `PalworldEditor loaded (v1.6.10)`；除物品、技能与世界切换回归外，逐项验证清单见
  `AGENTS.md` 的"验证一次改动"。

## 分支与协作流程

- `main` 仅用于发布；开发基线是 `develop`；
- 所有修改（功能、修复、文档、重构、脚本）都必须从 `develop` 切出独立分支，禁止直接在 `develop`
  或 `main` 上提交；分支名前缀：`feat/`、`fix/`、`docs/`、`chore/`、`refactor/`、`style/`、
  `test/`、`diag/`；
- 每个修改集 = 分支 → 推送远端 → 先提交 PR 到 `develop`；合入 `develop` 验证通过后，再按发布流程
  合入 `main`；
- 本地 `develop` 与 `main` 只用于 `pull` 对齐，不直接提交。

## C++ 编码规范与 doxygen 注释

以 `.clang-format`、`.clang-tidy`、`.editorconfig` 为准（行尾统一 LF、UTF-8、4 空格缩进），手工
编辑后必须与格式对齐：

- **格式**（`.clang-format`，Google 风格改编）：C++23、4 空格缩进、列宽 100、大括号 Attach、
  指针对齐 Left、`IncludeBlocks: Regroup`（std 头 → 尖括号头 → 引号头）。`format` / `format-check`
  CMake target 处理 `mods/` 下 Git 跟踪的 C/C++ 文件；
- **静态检查**（`.clang-tidy`）：启用 bugprone / cert / clang-analyzer / concurrency /
  cppcoreguidelines / misc / modernize / performance / portability / readability 各系列；UE4SS/
  Unreal 互操作相关的检查（C 风格转换、`reinterpret_cast`、非 const 全局等）属于刻意豁免。
  `tidy` / `tidy-check` target 运行于 `compile_commands.json` 中 `mods/` 下的翻译单元；
- **doxygen 注释**：公共头文件中的类型与函数必须有 `@file` + `@brief`（需要时补 `@details`）；
  参数用 `@param[in]` / `@param[out]`，返回值用 `@retval` / `@return`，行为约束与前提条件用
  `@note`。实现文件的关键路径同样写简短注释；
- **命名**：类型 PascalCase；函数/变量/成员 snake_case（成员尾部 `_`）；编译期常量 `k` 前缀
  （如 `kHotkeyDebounce`）；纯查询函数标记 `[[nodiscard]]`；
- **分层约束**：纯值领域层只依赖标准库（`<chrono>`、`<optional>`、`<string>` 等），不包含任何
  Unreal 头；Unreal 类型只在游戏线程适配层出现。

## 项目架构（脱离业务代码的整体架构）

### Super-build 布局

根 `CMakeLists.txt` 只做两件事：`add_subdirectory(RE-UE4SS)`（由 `scripts/setup.ps1` 克隆，已被
gitignore）与 `add_subdirectory(mods)`。RE-UE4SS 提供 `UE4SS` 静态库 target，传递性提供 mod 所需
的全部头文件、编译选项与宏定义（含 C++23）。`mods/` 下每个 mod 都是链接 `UE4SS` 的 `SHARED` 库；
新增 mod 可复制 `mods/PalworldEditor/`，修改 target 与运行时元数据，再在 `mods/CMakeLists.txt`
中 `add_subdirectory()`。

### UE4SS triplet 构建

RE-UE4SS 用自有的构建 triplet（`Game__Shipping__Win64`、`CasePreserving__Dev__Win64`…）驱动
`UE_GAME`、`UE_BUILD_SHIPPING`、`PLATFORM_WINDOWS` 等编译宏组合——只有当 `$<CONFIG>` 匹配某
triplet 时这些宏才生效。Ninja 是单配置生成器，preset 必须**显式**设置
`CMAKE_BUILD_TYPE=Game__Shipping__Win64`：RE-UE4SS 拉取的 imgui 依赖在其 examples 中把缺省
`CMAKE_BUILD_TYPE` 强制为 `Debug`，若不显式指定，关键宏不会定义，编译失败。这就是输出 DLL 落在
`build/Game__Shipping__Win64/bin/` 的原因。

### 入口点契约（`src/dllmain.cpp`）

- `PalworldEditorMod` 继承 `RC::CppUserModBase`：设置元数据，`on_update()` 保持为空，
  `on_unreal_init()` 注册 EngineTick 与 LoadMap 前/后回调；DLL 导出 `start_mod()`（构造实例）与
  `uninstall_mod()`（销毁实例）；
- 日志统一走 `RC::Output::send<LogLevel::Verbose>(STR("..."))`（底层是 `std::format`；`STR()` 选择
  正确的字符宽度）；
- 全局 EngineTick/LoadMap Hook 只在 `on_unreal_init()` 注册；业务 UFunction Hook 按需解析并注册，
  关闭或 LoadMap 前立即注销。

### 三层分层

1. **纯值领域层**（`inc/` 下 `*_editor.hpp`、`*_service.hpp`、`*_catalog.hpp`、`*_state.hpp` 等）：
   只依赖标准库，承载值、快照、请求、队列、校验、账本、帧预算等全部决策逻辑，可单元测试；
2. **游戏线程适配层**（`src/` 下 `pal_*.cpp`、`*_gateway.cpp`）：在 EngineTick 或对应 UFunction 的
   游戏线程回调内做反射查找与读写，把引擎状态翻译成纯值请求；
3. **UI 层**（`src/editor_ui.cpp` 与 `src/*_ui.cpp`）：ImGui 回调只做展示与输入，通过原子请求、
   互斥锁快照与游戏线程交接。

### 线程模型

ImGui 回调与游戏线程之间只传递标准库快照、互斥锁保护的请求参数与原子请求标志。UObject 反射读写
**只允许**在 EngineTick 或相应 UFunction 的游戏线程回调内执行；跨帧状态不得持有 UObject 指针或
Unreal 数组地址，解析得到的 `UObject*` 只在当次回调内使用。

### 工具链

clangd 读取 `build/compile_commands.json`（preset 已设 `CMAKE_EXPORT_COMPILE_COMMANDS=ON`），自动把
其中的 `cl.exe` 调用翻译为 clang-cl，无需单独 clang 工具链。修改 CMake 或新增源文件后，重新运行
`cmake --preset ninja-msvc-x64` 刷新该文件。clangd 若报 "system header not found" / 找不到
Windows SDK，允许它查询 MSVC 驱动（如
`"clangd.arguments": ["--query-driver=C:/Program Files/Microsoft Visual Studio/**/Hostx64/x64/cl.exe"]`）。

## 针对幻兽帕鲁游戏 mod 的架构及契约

本 mod 通过 `/Script/Pal.*` 函数路径与 Palworld 类型做反射调用，是 **Palworld 专用实现**；只有根
目录的 CMake/RE-UE4SS 脚手架适合扩展到其他 mod。业务实现遵循第 5 节的三层分层，以下只列模块职责
与必须遵守的契约。

### 业务模块

- `inc/game/pal_game.hpp`：背包、物品、当前待出战队伍帕鲁的反射访问，以及
  `TryGetSpawnedOtomoHandle` 出战状态检测；
- `inc/items/item_catalog.hpp`：本地化物品标签、搜索、去重与 Raw ID 索引；
- `inc/skills/`：主动/被动技能目录、分类规则、编辑服务、四词条预设、显式目标锁定与世界代次状态。
  主动技能数值/Raw ID 来自 `scripts/generate-active-skill-definitions.ps1` 从 UHT dump 生成的表，
  不读取运行时 `UEnum` 布局；更新 Palworld/UHT dump 后必须重新运行生成脚本；
- `inc/pal_stats/` + `src/pal_stats.cpp`：帕鲁属性编辑领域与 `SaveParameter` 反射适配；
- `inc/pal_identity/` + `src/pal_identity.cpp`：Alpha/Lucky/觉醒三维形态编辑；
- `inc/grappling_hook/` + `src/grapple_cooldown_gateway.cpp`：爪钩对象的一次性冷却覆盖与原值恢复；
- `inc/pal_remote_palbox/` + `src/pal_remote_palbox/`：远程终端——按键上升沿状态机（300ms 防连点）
  与基地选择策略（纯值层 `remote_palbox.hpp`）+ 游戏线程运行时 `remote_palbox_runtime.cpp`；
- `inc/base_resource_sharing/` + `src/pal_base_resources.*`、`src/pal_base_resource_runtime.*`：
  同公会跨据点制作/建造材料共享；
- `src/dllmain.cpp`：mod 生命周期、ImGui、EngineTick/LoadMap 与线程间请求交接；
- `src/*_ui.cpp`：各业务模块的 ImGui 界面。

### 反射与目标锁定契约

- 当前技能目标只在用户点击"选择当前帕鲁"后锁定；数字键切换不会自动切换编辑对象，提交修改前仍会
  重新校验当前 GUID；LoadMap 前必须清空请求并撤销写权限，进入新世界后必须重新选择；
- 被动技能分类由 `PalPassiveSkillManager:GetSkillData` 的 `Rank` 与 `AddWorldTreePal` 驱动：传说
  优先，其余按 `Rank` 划分负面、极品、稀有、普通。分类只由增量任务驱动，每个 EngineTick 最多读取
  8 个 ID 且受 500 微秒软预算约束；单个 `GetSkillData` 失败只标记未知，不终止任务。成功元数据缓存
  保留到卸载；LoadMap 前取消任务但保留缓存。分类完成前只有"全部"可选，结构性失败时若有旧分类则
  保留具体类别可用；
- 属性编辑：帕鲁之魂直接写入 Rank 字段并调用 `OnRep_SaveParameter` 刷新；浓缩同步
  `Rank`/`RankUpExp`。工作适应性以绝对永久附加值编辑、合计等级只读显示，提交时以相对当前值的有
  符号差值调用 `SetWorkSuitabilityAddRank` 增量写入，物种原本不具备的方向不能新增；工作适应性拥有
  独立安全停用域，不会因验证失败而禁用基础属性；
- 形态修改：Alpha 经原生数据库确认的普通/`BOSS_` CharacterID 配对切换，Lucky 写入 `IsRarePal`，
  觉醒写入 `bIsAwakening`，三者可任意组合且不消耗材料。只允许在帕鲁已收回时执行——出战判断以本地
  Holder `TryGetSpawnedOtomoHandle` 为准，不使用可能残留的 Actor 对象；写后重读失败时整笔回滚；
- 远程终端：圈内判定以世界设置 `BaseCampAreaRange`（视觉建造圈）为准，不使用随据点等级膨胀的据点
  模型 `AreaRange` 属性；战斗中禁用读取 `APalCharacter::bIsBattleMode` 属性（`IsInCombat` /
  `IsInBattle` 函数名在 Palworld 1.0 不存在）。

### 资源共享契约

资源目录通过 `PalBaseCampManager:GetBaseCampIds` / `TryGetModel`、`PalBaseCampModel.ModuleArray`、
`PalBaseCampModuleItemStorage.ContainerInfos` 和 `PalMapObjectManager:FindConcreteModel` 直接建立。
只接受同公会、已加载、类型为 `Chest` 的普通仓储。不得使用全局 `FindAllOf`，不得扫描或修改
`ItemSlotArray` / `StackCount`。

共享实现是世界代次内持续存在、可逆的公会仓储持久登记图：对每个同公会目标仓储模块，使用
`OnAvailableConcreteModel_ServerInternal` 登记其他据点已加载的普通箱子；目标据点自己的原生容器不
重复登记。制作和建造都只依赖 Palworld 原生仓储关系，不再修改本地主背包 `InventoryMultiHelper`，
也不再把联合绑定到建筑或制作菜单生命周期。菜单关闭、进入放置预览、连续建造和提交扣除期间不得恢复
或重建联合。

`OnAvailableConcreteModel_ServerInternal` 与 `OnNotAvailableConcreteModel_ServerInternal` 的
pre-hook 在参数仍由引擎持有时读取 ConcreteModel 身份，并只查询由上一次安全目录建立、完成登记/注销
后增量维护的排序纯值索引；不得在 Hook 内遍历模块或 `ContainerInfos`。post-hook 不解引用参数对象。
`PalBaseCampModel:OnRep_ModuleArray` 只在初始化明确等待仓储结构时唤醒，稳定状态下周期性的无变化
复制必须忽略。下一次 EngineTick 重新发现一次安全目录、剔除边账本中已注入到目标模块的容器，再计算
期望边与已应用边的最小差量，避免把本 Mod 的注入结果当成原生来源并递归扩张。新增/删除每帧最多执行
4 条，并受 500 微秒软预算限制。

每条由本 Mod 新增的边只保存目标/来源据点 GUID、容器 GUID、ConcreteModel 所有者 GUID 和目标模块
对象全名。登记前后必须验证目标容器为精确 0→1 且只在原序列尾部追加；异常立即调用原生注销接口回滚。
注销必须验证精确 1→0 且其他序列不变。恢复失败造成的本世界安全禁用不能通过切换开关绕过；关闭开关
仍必须进入 restoring 阶段尽力清理剩余账本。每次安全目录发现还必须把已发现目标模块中的实物登记与
边账本交叉验证。

本地权限门为 `IsServer && !IsDedicatedServer`。修理共享仍不可用。不要与 IntegratedStorage、
UBIM Lite、BlueprintResearch 或其他修改相同资源路径的 mod 同时测试。

### 部署契约

C++ mod 安装到游戏 `Pal/Binaries/Win64/ue4ss/Mods/<ModName>/dlls/main.dll`（把构建出的 DLL 改名；
用 `<ModName>.dll` 也可以）。启用方式：在 mod 文件夹里放一个空的 `enabled.txt`，**或**者在
`ue4ss/Mods/mods.txt` 中 `Keybinds` 行的上方加一行 `<ModName> : 1`。`deploy` target
（`cmake/Deploy.cmake`）通过 `$<TARGET_FILE:PalworldEditor>` 自动完成这件事，因此无论当前 triplet
输出目录是哪个，源文件路径都始终正确。
