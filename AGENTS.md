# AGENTS.md

This file provides guidance to Codex (Codex.ai/code) when working with code in this repository.

> 本文件为 Codex 在本仓库中工作时提供指引。

## 这是什么

一个面向 **Palworld 1.0** 的 **UE4SS C++ mod** 工程（C++23 / CMake / Ninja）。当前 mod 名为
`PalworldEditor`（版本 1.6.5），构建产物是 `PalworldEditor.dll`。

该 mod 通过 UE4SS GUI 提供物品浏览与修改、背包数量修改，以及数字键当前高亮、下一次按 E 会召唤的
队伍帕鲁主动/被动技能编辑；还提供默认关闭、仅面向单人/本地房主的同公会跨据点制作与建造材料共享。
mod 本体通过 `/Script/Pal.*` 函数路径和 Palworld 类型进行反射调用，因此是 Palworld 专用实现；只有根目录的
CMake/RE-UE4SS super-build 脚手架适合扩展其他 mod。

Palworld 1.0 需要 UE4SS 的 **experimental（实验版）**运行时（Steam 创意工坊里的
"UE4SS Experimental (Palworld)" + PalSchema，含 `MemberVariableLayout.ini`，或兼容的 GitHub release）。
F10 游戏控制台不可用，所有交互都通过 UE4SS GUI 中的 `PalworldEditor` 页签和浮动窗口完成。

## 前置依赖

- **Visual Studio 2022**（最新版），勾选 *"使用 C++ 的桌面开发"*（Desktop development with C++）工作负载——
  提供 MSVC（`cl.exe`）和 Ninja。C++23 通过 `/std:c++latest` 启用，因此需要较新的 VS 2022。
- **CMake ≥ 3.22**，在 PATH 中。
- **Git**，在 PATH 中（供 `scripts/setup.ps1` 使用）。
- **Rust stable（`cargo` / `rustc`）**。虽然 preset 关闭 `UE4SS_VERSION_CHECK` 且不构建独立 UVTD 程序，
  当前 RE-UE4SS 的 `UE4SS` target 仍会构建 Rust 实现的 PatternSleuth 依赖。

所有构建命令都必须在 **MSVC 环境**中运行——即 "x64 Native Tools Command Prompt for VS 2022" 或
VS Developer PowerShell——以保证 `cl.exe` 和 `ninja` 在 PATH 中。

## 常用命令

```powershell
# 1. 首次初始化：克隆 RE-UE4SS 并初始化子模块
pwsh scripts/setup.ps1

# 2.（一次性）把部署目标指向你的游戏安装目录
$env:PALWORLD_INSTALL_DIR = "C:\Program Files (x86)\Steam\steamapps\common\Palworld"
#   即包含 Pal/Binaries/Win64 的那个文件夹

# 3. 配置（请在 VS x64 开发者命令行中运行）
cmake --preset ninja-msvc-x64

# 4. 构建   -> build/Game__Shipping__Win64/bin/PalworldEditor.dll
cmake --build --preset ninja-msvc-x64 --target PalworldEditor

# 5. 构建并运行不链接 UE4SS 的纯 C++ 测试
cmake --build --preset ninja-msvc-x64 --target PalworldEditorTests
ctest --test-dir build --output-on-failure

# 6. 部署到游戏 -> Pal/Binaries/Win64/ue4ss/Mods/PalworldEditor/dlls/main.dll（+ enabled.txt）
cmake --build --preset ninja-msvc-x64 --target deploy

# 完全重新构建
Remove-Item -Recurse -Force build ; cmake --preset ninja-msvc-x64 ; cmake --build --preset ninja-msvc-x64
```

`PalworldEditorTests` 和 `PalworldEditorBaseResourceSharingTests`/CTest 覆盖不依赖 Unreal 的物品目录、
技能目录、技能编辑服务、配置、资源池、能力判断、恢复账本和生命周期逻辑。反射调用、ImGui 和 Palworld
存档效果仍需游戏内端到端验证。

## 架构

**Super-build 布局。** 根目录的 `CMakeLists.txt` 只做两件事：`add_subdirectory(RE-UE4SS)`（由
`scripts/setup.ps1` 克隆、已被 gitignore）和 `add_subdirectory(mods)`。RE-UE4SS 定义了 `UE4SS`
静态库 target，它会传递性地提供 mod 所需的全部头文件、编译选项和宏定义——包括 C++23 语言标准。
`mods/` 下每个 mod 都是一个链接 `UE4SS` 的 `SHARED` 库。当前 target 位于
`mods/PalworldEditor/CMakeLists.txt`；要新增其他 mod，可复制该目录、修改 target 和运行时元数据，再在
`mods/CMakeLists.txt` 中增加 `add_subdirectory()`。

**UE4SS 的三元组（triplet）构建系统 + Ninja。** RE-UE4SS 定义了它自己的构建 "triplet"
（`Game__Shipping__Win64`、`CasePreserving__Dev__Win64`……），由它们驱动编译宏定义（`UE_GAME`、
`UE_BUILD_SHIPPING`、`PLATFORM_WINDOWS` 等）的组合——只有当 `$<CONFIG>` 等于某个 triplet 时这些宏才会生效。
Ninja 是单配置（single-config）生成器，所以 preset **显式设置** `CMAKE_BUILD_TYPE=Game__Shipping__Win64`
（即 UE4SS 的默认值）。**必须显式设置**：RE-UE4SS 拉取的 imgui 依赖里，其 examples 含有
`if(NOT CMAKE_BUILD_TYPE) set(CMAKE_BUILD_TYPE Debug ... FORCE)`；若不显式指定，这个默认就会"赢"，使
`$<CONFIG>` 变成 `Debug` 而不匹配任何 triplet，UE4SS 的关键宏不会被定义，编译会失败。这就是输出 DLL 落在
`build/Game__Shipping__Win64/bin/` 的原因。

**PalworldEditor 内部分层。**

- `inc/game/pal_game.hpp`：背包、物品和帕鲁 UObject 反射访问；
- `inc/base_resource_sharing/settings.hpp`：默认关闭的配置解析与持久化接口；
- `inc/base_resource_sharing/resource_pool.hpp`：公会资源过滤/排序、能力与按注入次数恢复的纯逻辑；
- `inc/base_resource_sharing/resource_session.hpp`：8 秒目录校准调度与固定大小制作/建造操作会话；
- `inc/base_resource_sharing/hook_manifest.hpp`：Palworld 1.0.1 分阶段制作/建造 Hook 清单；
- `inc/base_resource_sharing/pal_base_resources.hpp` + `src/pal_base_resources.cpp`：事件驱动目录调度、
  制作/建造会话与 Hook 适配；
- `src/pal_base_resource_runtime.hpp` + `src/pal_base_resource_runtime.cpp`：通过游戏管理器发现同公会普通仓储、
  建立可逆临时资源联合并按账本恢复；
- `inc/items/item_catalog.hpp`：本地化物品标签、搜索、去重和索引；
- `inc/skills/active_skill_definitions.hpp`：生成的 Palworld 1.0 主动技能数值/Raw ID 表；
- `inc/skills/passive_skill_presets.hpp`：编译期四词条预设目录及单请求工厂；
- `inc/skills/skill_catalog.hpp`：可搜索的主动/被动技能目录、被动技能分类规则、增量分类任务与失败回退；
- `inc/skills/skill_editor_service.hpp`：编辑校验、FIFO 请求、重读和回滚；
- `inc/skills/selected_target_state.hpp`：显式锁定目标的一致性检测和过期编辑请求保护；
- `inc/skills/world_session_state.hpp`：LoadMap 世界代次、访问状态和逐世界目标确认；
- `inc/skills/pal_skills.hpp` + `src/pal_skills.cpp`：领域服务到 Palworld UFunction 的适配；
- `src/dllmain.cpp`：mod 生命周期、ImGui 和线程间请求交接。

**Mod 入口点契约**（`mods/PalworldEditor/src/dllmain.cpp`）：`PalworldEditorMod` 继承
`RC::CppUserModBase`，设置元数据并重写 `on_update`、`on_unreal_init`；`on_update()` 保持为空，
`on_program_start()` 读取资源共享配置，`on_unreal_init()` 注册 EngineTick 与 LoadMap 前/后回调；
DLL 导出 `start_mod()`（构造实例）
和 `uninstall_mod()`（销毁实例）。日志用
`RC::Output::send<LogLevel::Verbose>(STR("...{ }...\n"))`（底层是 std::format；`STR()` 会选择正确的字符
宽度）。全局 EngineTick/LoadMap Hook 只在 `on_unreal_init()` 注册；资源 UFunction Hook 仅在共享开启且
世界可访问时按需解析并注册，关闭共享或 LoadMap 前立即注销。对象查找、反射读写和 `ProcessEvent` 只允许在
EngineTick/对应 UFunction 游戏线程回调内执行。

ImGui 回调与游戏线程之间只传递标准库快照、互斥锁保护的请求参数和原子请求标志。所有 UObject 指针都视为
非拥有句柄；业务数据的反射读取和修改只在 EngineTick 游戏线程回调执行。当前技能目标从唯一属于本地
控制器的队伍 Holder 解析，用户点击“选择当前帕鲁”后以 `FPalInstanceID.InstanceId` 和目标代数锁定；
只有再次点击该按钮才会切换编辑目标。数字键切换不自动清空选择；空闲时无论是否已确认目标都不执行当前
帕鲁解析。选择和编辑请求会在同一 EngineTick 立即解析，编辑消费前仍会重新校验当前 GUID，不一致或瞬时
解析失败时拒绝写入。解析得到的 UObject 只在当次回调内使用，技能 GUI 快照仅在
可观察值变化时发布。不缓存扫描得到的帕鲁对象，也不注册详情页函数 Hook。LoadMap 前置回调递增世界代次、
清空所有待处理操作并撤销写权限；后置回调只恢复读取和目录刷新。原目标 GUID/名称仅用于显示，进入新世界后
必须再次点击“选择当前帕鲁”，技能编辑请求还必须匹配提交时的世界代次。

主动技能目录不读取运行时 `UEnum` 内存布局，而是使用
`scripts/generate-active-skill-definitions.ps1` 从 Palworld 1.0 UHT dump 生成的数值/Raw ID 表。
主动和被动名称由 `PalUIUtility` 按游戏当前语言查询，`PalPlayerInventoryData` 只作为当帧本地化世界上下文；
上下文暂不可用时目录回退为 Raw ID。两个目录区段分别维护可用状态、错误和旧目录回退，一类失败不禁用另一类。
启动时物品扫描属于初始化工作；技能目录及本地化反射必须等待玩家 Common 主背包容器有效，并且只在现有
2 秒刷新到期或手动请求时检查该安全门。手动刷新不能绕过安全门，检查得到的容器指针不得离开当次 EngineTick。
这些初始化工作不是常驻逐帧解析。更新 Palworld/UHT dump 后必须重新运行生成脚本。

被动技能分类选择器在新增/替换流程中以“类别 + 技能”两级下拉框呈现，分类来源是
`PalPassiveSkillManager:GetSkillData` 的 `Rank` 与 `AddWorldTreePal`：`AddWorldTreePal` 为真判定传说，
否则按 `Rank` 划分负面（<0）、极品（≥4）、稀有（3）和普通。分类只在被动目录成功刷新后由增量任务驱动，每个
EngineTick 最多读取 8 个 ID 且受 500 微秒软预算约束，并在每次 `ProcessEvent` 后检查时间。单个 `GetSkillData`
返回假只把该技能标为未知（仅出现在“全部”），不终止任务；成功读取的 `{Raw ID -> 元数据}` 纯值缓存保留到 mod
卸载，手动刷新只重试新 ID 与先前失败 ID。分类完成前只有“全部”可选；任务的结构性错误若发生在已有可用分类之后，
界面保留旧具体类别可用并提示“正在使用上一次成功分类”。LoadMap 前取消任务、撤销分类写权限但保留成功缓存。
中文名与 Raw ID 搜索在所有类别中生效；切换类别清空已选技能但保留搜索文本。1.6.5 增加该选择器，不改变主动
技能目录、被动写入方式、四词条预设、目标锁定规则与资源共享实现，也不引入常驻扫描或逐帧任务。

跨据点资源共享通过 `PalBaseCampManager:GetBaseCampIds` / `TryGetModel` 读取同公会据点，再从
`PalBaseCampModel.ModuleArray` 的 `PalBaseCampModuleItemStorage.ContainerInfos` 筛选 `Chest` 类型普通仓储；
每个登记项都必须通过 `PalMapObjectManager:FindConcreteModel` 解析到已加载的 `PalItemContainer`。该功能
不使用全局 `FindAllOf`，不扫描或写入 `ItemSlotArray`/`StackCount`，也没有一秒一次的物品数量预览缓存。

`OnRep_ContainerInfos`、仓储 ConcreteModel 可用性和 `OnRep_ModuleArray` 只负责合并目录失效标记；活动材料
会话期间目录调度器不得执行校准或拆装联合，会话结束后处理一次失效，8 秒校准仅为空闲兜底。
`PalUIBuildModel:OnOpenMenu` 与 `PalUIConvertItemModel:Initialize` 的 pre-hook 在原版首次资格计算前获取
会话并建立联合。制作只向本地主背包 `InventoryMultiHelper` 追加同公会普通箱子，避免与据点模块重复计数；
建造同时扩展据点仓储模块和材料助手。高频列表、配方和资格 Hook 只刷新固定大小状态，不做 UObject 查找、
反射、目录发现、数组修改或日志。`StartProduction` post-hook 在原请求返回后释放制作会话，1.5 秒只作为未提交
请求的空闲兜底；退出建造模式释放建造会话。恢复按原始次数、注入次数和当前序列执行，运行时新增的非注入引用
会保留。首次菜单早于目录初始化时保存对象全名并在下一 EngineTick 完成一次目录发现和联合，不允许逐帧重试。
跨帧只持有 GUID、对象全名和标准库账本，不持有 Unreal 对象或数组地址。

本地权限必须满足 `IsServer && !IsDedicatedServer`。关闭开关、LoadMap 前置和卸载都先恢复活动联合再注销
资源 Hook。制作、建造、修理能力独立失败关闭；修理在 Palworld 1.0.1 中保持不可用。Verbose 日志只记录目录
校准、联合建立及恢复耗时。配置位于 `ue4ss/Mods/PalworldEditor/config.ini`，仅包含
`[BaseResourceSharing]` 下的 `Enabled=true|false`。不要与 IntegratedStorage、UBIM Lite、
BlueprintResearch 或等价的资源路径 mod 同时启用。

同一可访问世界内从关闭切换为开启时，资源桥必须按当前世界代次重新初始化会话和目录调度器，并由后续
EngineTick 自动执行一次既有的管理器目录校准。不得在 GUI 回调中扫描，也不得为重新开启增加线程、全局
UObject 扫描、槽位扫描或逐帧任务。恢复失败造成的本世界安全禁用不能通过切换开关绕过。1.6.1 修复该
开关生命周期；1.6.2 移除技能目标的空闲后台解析；1.6.3 实现首次资格计算前的资源联合、制作唯一 Helper
入口和活动会话校准抑制；1.6.4 增加被动技能四词条预设的差量应用和失败回滚，不增加逐帧工作。

**部署契约。** C++ mod 安装到游戏 `Pal/Binaries/Win64/ue4ss/Mods/<ModName>/dlls/main.dll`（把构建出的
DLL 改名；用 `<ModName>.dll` 也可以）。启用方式：在 mod 文件夹里放一个空的 `enabled.txt`，**或**者在
`ue4ss/Mods/mods.txt` 中 `Keybinds` 行的上方加一行 `<ModName> : 1`。`deploy` target（`cmake/Deploy.cmake`）通过
`$<TARGET_FILE:PalworldEditor>` 自动完成这件事，因此无论当前 triplet 输出目录是哪个，源文件路径都始终正确。

## 工具链（clangd / clang-tidy / clang-format）

`.clangd`、`.clang-tidy`、`.clang-format`、`.editorconfig` 和 `.gitattributes` 负责编辑器内的分析与格式化；
行尾统一为 LF。

clangd 读取 `build/compile_commands.json`（preset 里设置了 `CMAKE_EXPORT_COMPILE_COMMANDS=ON`）。本工程用
MSVC 构建，所以该文件里记录的是 `cl.exe` 调用，clangd 会自动把它们翻译成自己的 clang-cl 前端——不需要单独的
clang 工具链。修改 `CMakeLists.txt` 或新增源文件后，请重新运行 `cmake --preset ninja-msvc-x64` 来刷新它。

- clang-tidy 通过 `.clang-tidy` 在 clangd 内部运行；target 只选择 `mods/` 下的翻译单元，第三方头文件仍会
  被解析，但 `HeaderFilterRegex: 'mods[/\]'` 会抑制其常规诊断。Windows 下 `tidy-check` 是单进程批量执行，
  解析 RE-UE4SS/Unreal 头文件可能耗时较长。
- `.clang-format` = Allman 大括号、4 空格缩进、120 列上限（与现有代码一致）。
- 如果 clangd 报 "system header not found" / 找不到 Windows SDK，请允许它查询 MSVC 驱动——例如 VS Code：
  `"clangd.arguments": ["--query-driver=C:/Program Files/Microsoft Visual Studio/**/Hostx64/x64/cl.exe"]`。

## 验证一次改动

提交前至少执行：

```powershell
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor PalworldEditorTests PalworldEditorBaseResourceSharingTests
ctest --test-dir build --output-on-failure
git diff --check
```

构建并部署后启动 Palworld 1.0。UE4SS 控制台应出现 `PalworldEditor loaded (v1.6.5)`；打开 UE4SS GUI 的
`PalworldEditor` 页签后应能看到浮动窗口。至少验证物品扫描与本地化标签、背包读取、数字键高亮队伍帕鲁后点击
“选择当前帕鲁”、切换高亮目标时保持锁定但暂停写入、启动后自动加载完整技能目录、点击“刷新技能列表”
不崩溃、两个技能下拉框都可选择、
主动/被动名称跟随游戏语言、已装备主动技能数值可映射为标签、被动技能新增/替换/删除且可按类别筛选并着色、
分类完成前仅“全部”可选而分类后五个类别可切换、两个四词条预设
只在点击“应用预设”后执行且可差量写入/失败回滚，以及主动技能
装备/替换/清空。场景中保留一只野生帕鲁时，编辑目标仍必须是下一次按 E 会召唤的队伍帕鲁。若 mod 未加载，
检查安装路径、`dlls/main.dll` 命名，以及 `enabled.txt`/`mods.txt`。还应重复退出世界/重进存档，确认
加载期间请求被清空、原目标仅保留显示、重新选择前无法写入，并且 LoadMap 不再崩溃。还应确认无论是否已确认
目标，空闲等待至少 10 秒都不再解析队伍 Holder；数字键切换不会静默改变锁定目标，提交修改时会立即重查并拒绝
错误目标。实际帧时间改善必须在游戏内测量。资源共享还应验证：
默认关闭且配置可持久化；关闭时工厂/建造界面性能与未启用资源功能一致；开启后反复打开工厂和建造菜单不再
持续卡顿，另一据点箱子中的材料变化能由原生容器引用直接反映到预览；首次打开建筑菜单时图标即可选择，
无需先打开炉子；制作最大数量与真实可制作数量一致且不会把同一箱子计算两次；制作/建造能消费同公会另一
已加载据点的普通箱子材料；
材料不足时不扣除；关闭开关与 LoadMap 后恢复原版行为；食物箱、运输、自动生产和箱子 UI 不共享；修理明确
显示不可用；同一世界内关闭后重新开启会自动恢复非零计数且每次只产生一次成功的初始校准；日志中目录校准
仅在无活动材料会话时最多每 8 秒一次，活动菜单期间不出现联合反复恢复/重建；每次实时联合都有匹配且无错误
的恢复和耗时记录。不要与
IntegratedStorage、UBIM Lite、BlueprintResearch 等修改相同资源路径的 mod 同时测试。

还应从桌面连续冷启动游戏多次，确认进入主界面前不会调用技能目录反射导致崩溃；进入存档、Common 主背包
就绪后目录应自动加载当前语言名称，手动刷新仍能正常工作。1.6.4 保留技能目录启动安全门和按需帕鲁解析，
并把材料联合提前到原版资格计算前；资源开关在同一世界内重新开启时仍会重启目录调度器。1.6.5 增加被动技能分类
选择器，分类来源是 `GetSkillData` 的 `Rank`/`AddWorldTreePal`，以有界小批次在 EngineTick 后台完成，分类完成前
仅“全部”可选，手动刷新复用成功缓存，不增加常驻扫描或逐帧工作。

## 权威参考资料

- 官方模板：https://github.com/UE4SS-RE/UE4SSCPPTemplate
- 创建 C++ mod：https://docs.ue4ss.com/guides/creating-a-c++-mod.html
- 安装 C++ mod：https://docs.ue4ss.com/dev/guides/installing-a-c++-mod.html
- RE-UE4SS（框架 + 构建系统）：https://github.com/UE4SS-RE/RE-UE4SS
- Palworld 1.0 运行时：https://github.com/UE4SS-RE/RE-UE4SS/releases ,
  https://steamcommunity.com/workshop/filedetails/?id=3625223587
