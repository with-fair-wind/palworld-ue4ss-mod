# CLAUDE.md

本文件为 Claude Code 在本仓库工作提供指引。面向最终用户的说明见 `README.md`；面向 Codex 的仓库级
指引见 `AGENTS.md`（三者保持对齐，`AGENTS.md` 保留最完整的验证清单）。

## 项目概览

这是一个面向 **Palworld 1.0** 的 **UE4SS C++23 mod** 工程（CMake / Ninja super-build）。当前 mod
名为 `PalworldEditor`（版本 1.7.0），构建产物为 `PalworldEditor.dll`。兼容基线为 Palworld 1.0；
正文中出现的 1.0.1 等补丁号说明仅作历史行为记录，不表示声明支持多个补丁版本。Palworld 的 F10 游戏控制台
不可用，所有用户交互都通过 UE4SS GUI 的 ImGui 浮动窗口完成。

提供功能一览：

- **物品**：运行时物品目录、本地化搜索、给予物品与主背包数量修改；
- **技能**：数字键高亮"下一次按 E 会召唤"的队伍帕鲁，主动/被动技能编辑、被动技能分类选择与四词条预设；
- **帕鲁属性**：等级、个体值、四项帕鲁之魂强化、浓缩星级、性别、13 类工作适应性永久加成与亲密度；
- **形态**：Alpha（头目）、Lucky（闪光）、觉醒三个独立开关，需收回帕鲁后应用；
- **队伍复活**：按当前队伍槽位恢复帕鲁生命状态；
- **爪钩与捕获**：默认关闭的爪钩无冷却，以及投球期间瞬时捕获限制覆盖；
- **复活计时**：默认关闭的终端复活等待移除（可逆，关闭/切图恢复原值）；
- **标记传送**：按键传送到最近的自定义地图标记（F7 默认，可配置门控与到达高度）；
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
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor PalworldEditorTests PalworldEditorCommonTests PalworldEditorModLifecycleTests PalworldEditorBaseResourceSharingTests PalworldEditorRemotePalboxTests PalworldEditorCaptureOverrideTests PalworldEditorReviveTimerTests PalworldEditorWaypointTeleportTests
ctest --test-dir build --output-on-failure
git diff --check
```

- 八个测试 target 均为**不链接 UE4SS 的纯 C++ 测试**，覆盖参数方向判定、卸载清理调度（有界重试、
  永久失败销毁阻断）、物品目录、
  技能目录与编辑、配置、资源共享、远程终端、捕获覆盖、复活计时和标记传送等纯值逻辑；反射调用、
  ImGui 与 Palworld 存档效果仍需游戏内端到端验证；
- 游戏内应看到 `PalworldEditor loaded (v1.7.0)`；除物品、技能与世界切换回归外，逐项验证清单见
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

### Palworld 运行时反射安全编码规则（强制）

- **运行时元数据是调用依据**：UHT/SDK dump 只用于确认函数名、属性名和生成静态定义，不能作为当前
  进程内存布局仍然兼容的证明。每个 `ProcessEvent` 调用前都必须从实际 `UFunction` 校验完整签名；
  缺少函数、属性或类型不匹配时 fail-closed，不得尝试兼容性猜测或部分调用；
- **禁止手写固定参数布局**：新增或修改反射调用时，不得声明本地 `struct Params` 后直接把地址传给
  `ProcessEvent`，也不得用 `reinterpret_cast`、固定偏移、`memcpy` 模拟参数结构。统一使用
  `pal_game::FunctionParams` 按 `GetParmsSize()` 分配，并由 `InitializeStruct` / `DestroyStruct` 以 RAII
  管理其中的 `FString`、`TArray`、结构体等非平凡字段；
- **签名必须精确验证**：调用前使用 `has_exact_parameter_count` 校验全部 `CPF_Parm` 数量（包括返回
  值），再按属性名、输入/输出/返回方向和具体 `FProperty` 子类逐项验证。整数必须匹配宽度与有无符号，
  枚举必须验证底层数值属性，对象必须验证 `FObjectPropertyBase`，结构体应验证身份与大小，数组还必须
  验证 Inner 类型和不受支持的 allocator 标志；
- **正确解释 Unreal 参数标志**：`const T&` 可能同时带 `CPF_ConstParm`、`CPF_ReferenceParm` 与
  `CPF_OutParm`，语义仍是只读输入；不得把任意 `OutParm` 直接认作可写输出。输入、输出与返回值统一
  通过 `is_input_parameter` / `is_output_parameter` / `is_return_parameter` 和预期属性类型共同判断；
- **通过属性 API 读写**：输入使用 `SetPropertyValueInContainer` 或 `CopyCompleteValue`，输出和返回值
  使用对应 Property getter；动态数组读取使用 `FScriptArrayHelper_InContainer`，不得把参数缓冲区强转为
  `TArray<T>`。数组写路径（Add/Remove）因本 UE4SS 构建未导出
  `FMemoryImageAllocatorBase::ResizeAllocation`（helper 的 freezable 分支无法链接）而按 property 的
  元素大小/对齐直接调用 `FScriptArray::Add/Remove` 并配 `InitializeValue`/`DestroyValue`（与 helper
  堆数组路径语义一致）。读取数组前验证 `Num()` 非负且不超过领域上限，完成范围校验后才能 `reserve`、
  遍历或做窄化转换；
- **反射对象都是短期非拥有句柄**：`UObject*`、`UFunction*`、`FProperty*` 与 Unreal 容器地址只在
  当前游戏线程调用链内有效；跨帧只保存 GUID、对象全名和标准库纯值。每次 `ProcessEvent` 后若还要
  继续修改，必须重新解析或至少重新验证目标；对象返回值在使用前再次执行 `pal_game::is_valid`；
- **反射边界不传播异常**：适配层使用 `std::optional`、明确的结果结构或状态枚举报告失败，不让 C++
  异常穿过 UE4SS/Unreal Hook。输出参数在入口先清空，失败不得留下看似可用的半成品结果。

### 反射写事务与回滚规则（强制）

- 所有多步写操作采用“预检并读取原值 → 最小差量写入 → 调用原生 setter/OnRep 通知 → 完整重读验证”
  的顺序；任一步缺少读取能力或签名验证失败时，禁止开始写入；
- 写入后必须比较可观察值，而不是仅以 `ProcessEvent` 已返回作为成功依据。失败时只恢复本事务实际改动
  的字段或数组尾部，再重新通知并验证原快照；不得用清空整个容器等扩大影响面的方式回滚；
- 结果必须区分预检失败、写入成功、已验证回滚和回滚失败。回滚无法验证时保留恢复责任，并安全停用
  对应功能域；不得通过重复点击或切换开关绕过安全停用；
- 基础属性、工作适应性、形态、技能和资源共享等事务保持独立安全域。一个领域失败不能静默污染其他
  领域，也不能把旧快照发布为本次成功结果。

### 性能优化与封装规则

- UObject 查找、函数解析、目录扫描和 `ProcessEvent` 只放在游戏线程的显式请求、初始化安全门或结构
  事件路径；空闲 EngineTick 和 UI 绘制不得反射轮询。批量工作必须设置每帧条数上限与软时间预算，并
  在每次 `ProcessEvent` 后检查预算；
- 跨线程、跨帧缓存只保存经过验证的标准库纯值；不得为减少查找而缓存 UObject、UFunction、FProperty
  或 Unreal 数组地址。低频失败采用有上限、可停止的重试，成功后立即取消；稳定状态不保留定时扫描；
- 只抽取稳定且重复的底层原语：参数 RAII、签名/类型验证和基地管理器等单次公共调用可共享；领域层的
  容错、事务与诊断继续留在各模块。禁止建立可执行任意反射调用的庞大通用框架或“通用事务引擎”；
- 同一实体的关联字段使用单个值结构和单个容器，禁止依靠多个并行 `vector` 的相同索引维持关系。先做
  数量上限校验再预留容量，优先差量更新、值比较和事件驱动，避免无变化快照、重复日志与全量重建；
- 优化必须有游戏内帧时间、调用次数或日志计数依据。没有测量数据时，不新增目录缓存层、UI 过滤缓存或
  主动/被动接口层级；低频代码优先保持直接、可审计，避免以抽象数量代替实际性能收益。

### 新增功能的模块化与复用规则（强制）

- **实现前先做能力盘点**：新增类型、helper、反射适配器、状态机、缓存、配置解析或 UI 控件前，必须用
  `rg` 检查 `inc/`、`src/`、测试、CMake 和本文档中是否已有同类能力。先记录可直接复用、可小幅扩展
  和必须新增的部分；不得通过改名或换目录复制已有逻辑；
- **按三层架构确定唯一归属**：可脱离 Unreal 的规则、状态、请求、快照和算法放在 `inc/<feature>/`；
  UObject 查找、反射读写、Hook 和事务适配放在 `src/<feature>/` 或对应 gateway；ImGui 文件只负责展示
  与收集输入。`dllmain.cpp` 只做生命周期注册、游戏线程编排和模块间请求交接，不得新增领域判断、复杂
  反射流程或大段 UI；
- **依赖只能朝既定方向流动**：纯值领域层不得包含 Unreal/UE4SS/ImGui 头；UI 与其他线程不得直接调用
  `ProcessEvent`；游戏适配层把 Unreal 状态转换为标准库值后再进入领域层。模块之间通过小型、强类型的
  请求/结果/快照接口协作，禁止共享可变全局状态、跨模块访问内部容器或形成循环 include；
- **单一状态所有者**：每项功能的运行状态、世界代次、待处理请求、错误和能力标志必须有唯一所有者。
  UI 显示值从发布快照派生，不另建第二套可写状态；不要在多个 bool、并行容器或不同模块中重复表达同一
  阶段。需要一起传递的关联字段组合成领域结构，返回多个结果时使用命名结果结构而非松散输出参数；
- **优先复用现有最小原语**：对象有效性、`FunctionParams`、签名验证、文本编码、基地反射、目标锁定、
  生命周期门、请求/快照交接和帧预算等已有能力必须优先调用。现有接口只缺少一种受支持类型或一个稳定
  操作时，扩展原组件并补测试，不得在新模块私建近似 helper；
- **抽取公共代码必须语义相同**：只有当两个以上模块具有相同输入输出、线程/生命周期前提、错误语义和
  安全策略时，才抽取公共底层组件。仅代码形状相似但容错或事务不同的流程保留领域适配器，共享更底层
  的单次操作；公共层不得通过大量开关、回调或模板参数同时承载互斥业务；
- **防止公共目录变成杂项箱**：`common/` 只接收无业务归属、契约稳定、被多个模块实际使用的原语；
  单模块 helper 留在该模块的匿名 namespace 或私有实现文件。禁止新增 `utils.hpp`、`helpers.hpp`、
  `manager`、`service locator` 等边界模糊的万能入口；公共 API 保持最小，自包含头文件不泄露实现细节；
- **生命周期接入必须成对**：新 Hook、缓存、事务账本和运行时开关必须明确初始化、LoadMap 前清理、
  LoadMap 后恢复条件、关闭和卸载路径；注册与注销、应用与恢复必须由同一模块负责。不得把清理责任留给
  调用者猜测，也不得因新增功能改变其他模块的安全停用域；
- **优先扩展现有编排，不新增基础设施**：一个功能不得单独引入事件总线、插件注册表、通用仓库层、通用
  状态机或通用事务框架。先用现有请求标志、快照发布、EngineTick 初始化任务和模块私有流程实现；只有
  多个已存在功能出现稳定且可测试的共同契约，并能实际删除重复代码时才升级抽象；
- **测试与构建属于模块的一部分**：纯值决策必须加入现有对应测试 target，或在确有独立生命周期和依赖
  时新增测试 target；新增源文件同步更新 `mods/PalworldEditor/CMakeLists.txt`。反射/Hook 无法纯测的
  路径必须记录游戏内验证项。完成前检查没有把 UObject 带入纯值头、没有重复 helper、没有新增空闲帧
  轮询、没有遗留未对称清理，并运行格式、相关测试与端到端验证。

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

### 入口点契约（`src/mod/dllmain.cpp`）

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
   （例外：`inc/common/hotkey_capture_ui.hpp` 为被远程终端与标记传送两个 UI 共用的 header-only
   ImGui 原语，不接触反射，按既有归属保留在 common/；`game_foreground.hpp`/`text_encoding.hpp`
   为含 Windows.h 的平台工具原语，同属文档化例外。）
2. **游戏线程适配层**（`src/` 下 `pal_*.cpp`、`*_gateway.cpp`）：在 EngineTick 或对应 UFunction 的
   游戏线程回调内做反射查找与读写，把引擎状态翻译成纯值请求；
3. **UI 层**（`src/mod/editor_ui.cpp` 与各业务模块 `src/*/*_ui.cpp`）：ImGui 回调只做展示与输入，通过原子请求、
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
- `inc/pal_stats/` + `src/pal_stats/pal_stats.cpp`：帕鲁属性编辑领域与 `SaveParameter` 反射适配；
- `inc/pal_identity/` + `src/pal_identity/pal_identity.cpp`：Alpha/Lucky/觉醒三维形态编辑；
- `inc/pal_revive/` + `src/pal_revive/pal_revive.cpp`：队伍帕鲁复活；
- `inc/grappling_hook/` + `src/grappling_hook/grapple_cooldown_gateway.cpp`：爪钩对象的一次性冷却覆盖与原值恢复；
- `inc/capture_override/` + `src/capture_override/`：捕获限制瞬时事务与 Hook 生命周期；
- `inc/revive_timer/` + `src/revive_timer/`：终端复活计时移除与单字段恢复账本；
- `inc/waypoint_teleport/` + `src/waypoint_teleport/`：传送至最近自定义地图标记（配置解析、最近选择、无扫掠放置）；
- `inc/pal_remote_palbox/` + `src/pal_remote_palbox/`：远程终端——按键上升沿状态机（300ms 防连点）
  与基地选择策略（纯值层 `remote_palbox.hpp`）+ 游戏线程运行时 `remote_palbox_runtime.cpp`；
- `inc/base_resource_sharing/` + `src/base_resource_sharing/pal_base_resources.*`、
  `src/base_resource_sharing/pal_base_resource_runtime.*`：同公会跨据点制作/建造材料共享；
- `inc/common/` + `src/common/`：反射参数、签名判断与 UFunction Hook 登记等公共原语；
- `src/mod/dllmain.cpp`：mod 生命周期、ImGui、EngineTick/LoadMap 与线程间请求交接；
- `src/*/*_ui.cpp`：各业务模块的 ImGui 界面。

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
  `IsInBattle` 函数名在 Palworld 1.0 不存在）。地牢/骑乘/战斗门控启用时，对应状态不可读取按拦截
  处理（fail-closed），不视为安全放行。

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
