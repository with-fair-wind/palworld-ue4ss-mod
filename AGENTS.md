# AGENTS.md

This file provides guidance to Codex (Codex.ai/code) when working with code in this repository.

> 本文件为 Codex 在本仓库中工作时提供指引，结构与 `CLAUDE.md` 对齐并保留最完整的验证清单。

## 项目概览

一个面向 **Palworld 1.0** 的 **UE4SS C++ mod** 工程（C++23 / CMake / Ninja）。当前 mod 名为
`PalworldEditor`（版本 1.7.0），构建产物是 `PalworldEditor.dll`。

该 mod 通过 UE4SS GUI 提供物品浏览与修改、背包数量修改，数字键当前高亮、下一次按 E 会召唤的队伍
帕鲁主动/被动技能编辑（含被动分类与四词条预设）、属性编辑、Alpha/Lucky/觉醒形态修改、队伍复活、
远程终端，以及默认关闭的爪钩无冷却、捕获限制覆盖、终端复活计时移除、标记点传送和仅面向单人/
本地房主的同公会跨据点制作与建造材料共享。mod 本体通过 `/Script/Pal.*`
函数路径和 Palworld 类型进行反射调用，因此是 Palworld 专用实现；只有根目录的 CMake/RE-UE4SS
super-build 脚手架适合扩展其他 mod。

Palworld 1.0 需要 UE4SS 的 **experimental（实验版）**运行时（Steam 创意工坊里的
"UE4SS Experimental (Palworld)" + PalSchema，含 `MemberVariableLayout.ini`，或兼容的 GitHub
release）。F10 游戏控制台不可用，所有交互都通过 UE4SS GUI 中的 `PalworldEditor` 页签和浮动窗口
完成。

## 环境搭建及构建与验证

### 前置依赖

- **Visual Studio 2022**（最新版），勾选 *"使用 C++ 的桌面开发"*（Desktop development with C++）
  工作负载——提供 MSVC（`cl.exe`）和 Ninja。C++23 通过 `/std:c++latest` 启用，因此需要较新的
  VS 2022；
- **CMake ≥ 3.22**，在 PATH 中；
- **Git**，在 PATH 中（供 `scripts/setup.ps1` 使用）；
- **Rust stable（`cargo` / `rustc`）**。虽然 preset 关闭 `UE4SS_VERSION_CHECK` 且不构建独立 UVTD
  程序，当前 RE-UE4SS 的 `UE4SS` target 仍会构建 Rust 实现的 PatternSleuth 依赖。

所有构建命令都必须在 **MSVC 环境**中运行——即 "x64 Native Tools Command Prompt for VS 2022" 或
VS Developer PowerShell——以保证 `cl.exe` 和 `ninja` 在 PATH 中。

### 常用命令

```powershell
# 1. 首次初始化：克隆 RE-UE4SS 并初始化子模块
pwsh scripts/setup.ps1

# 2.（一次性）把部署目标指向你的游戏安装目录（须在首次配置前设置）
$env:PALWORLD_INSTALL_DIR = "C:\Program Files (x86)\Steam\steamapps\common\Palworld"

# 3. 配置（请在 VS x64 开发者命令行中运行）
cmake --preset ninja-msvc-x64

# 4. 构建   -> build/Game__Shipping__Win64/bin/PalworldEditor.dll
cmake --build --preset ninja-msvc-x64 --target PalworldEditor

# 5. 构建并运行不链接 UE4SS 的纯 C++ 测试
cmake --build --preset ninja-msvc-x64 --target PalworldEditorTests PalworldEditorBaseResourceSharingTests PalworldEditorRemotePalboxTests PalworldEditorCaptureOverrideTests PalworldEditorReviveTimerTests PalworldEditorWaypointTeleportTests
ctest --test-dir build --output-on-failure

# 6. 部署到游戏 -> Pal/Binaries/Win64/ue4ss/Mods/PalworldEditor/dlls/main.dll（+ enabled.txt）
cmake --build --preset ninja-msvc-x64 --target deploy

# 完全重新构建
Remove-Item -Recurse -Force build ; cmake --preset ninja-msvc-x64 ; cmake --build --preset ninja-msvc-x64
```

部署 target 依赖 `PALWORLD_INSTALL_DIR`；配置完成后修改该变量需重新运行
`cmake --preset ninja-msvc-x64`。游戏运行时会锁定已部署的 `main.dll`，部署前需退出游戏。

### 验证一次改动

提交前至少执行：

```powershell
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor PalworldEditorTests PalworldEditorBaseResourceSharingTests PalworldEditorRemotePalboxTests PalworldEditorCaptureOverrideTests PalworldEditorReviveTimerTests PalworldEditorWaypointTeleportTests
ctest --test-dir build --output-on-failure
git diff --check
```

六个测试 target/CTest 覆盖不依赖 Unreal 的物品目录、技能目录、技能编辑服务、配置、资源池、能力
判断、恢复账本、远程终端、捕获覆盖、复活计时与标记传送决策和生命周期逻辑。反射调用、ImGui 和 Palworld 存档效果仍需
游戏内端到端验证。

构建并部署后启动 Palworld 1.0。UE4SS 控制台应出现 `PalworldEditor loaded (v1.7.0)`；打开
UE4SS GUI 的 `PalworldEditor` 页签后应能看到浮动窗口。至少验证：物品扫描与本地化标签、背包读取、
数字键高亮队伍帕鲁后点击"选择当前帕鲁"、切换高亮目标时保持锁定但暂停写入、启动后自动加载完整技能
目录、点击"刷新技能列表"不崩溃、两个技能下拉框都可选择、主动/被动名称跟随游戏语言、已装备主动技能
数值可映射为标签、被动技能新增/替换/删除且可按类别筛选并着色、分类完成前仅"全部"可选而分类后五个
类别可切换、两个四词条预设只在点击"应用预设"后执行且可差量写入/失败回滚，以及主动技能装备/替换/
清空。属性编辑还应验证四项帕鲁之魂强化均可在 0–20（包括两个边界）内独立修改；浓缩 0 星和运行时
最大星级、雄性/雌性均可独立写入并在重读后保持，伙伴技能等级随浓缩 Rank 更新；工作适应性只强化物种
原有方向，以绝对永久附加值编辑并由独立按钮和安全域提交，合计等级只读显示。点击应用前不写入、不消耗
帕鲁之魂，重新召唤或重开详情页后值仍保持；任一字段不可访问或写后重读不一致时必须整笔拒绝或回滚。
形态的"正在场上"判断必须比较本地 Holder `TryGetSpawnedOtomoHandle` 与选中 Handle，不得使用可能
残留的 Actor。场景中保留一只野生帕鲁时，编辑目标仍必须是下一次按 E 会召唤的队伍帕鲁。若 mod 未
加载，检查安装路径、`dlls/main.dll` 命名，以及 `enabled.txt`/`mods.txt`。还应重复退出世界/重进
存档，确认加载期间请求被清空、原目标仅保留显示、重新选择前无法写入，并且 LoadMap 不再崩溃。还应
确认无论是否已确认目标，空闲等待至少 10 秒都不再解析队伍 Holder；数字键切换不会静默改变锁定目标，
提交修改时会立即重查并拒绝错误目标。远程终端还应验证：圈内判定以世界设置 `BaseCampAreaRange`
（视觉建造圈）为准，不使用随据点等级膨胀的据点模型 `AreaRange`；战斗中禁用读取
`APalCharacter::bIsBattleMode` 属性。资源共享还应验证：默认关闭且不跨进程持久化；关闭时工厂/建造
界面性能与未启用资源功能一致；开启后反复打开工厂和建造菜单不再持续卡顿，另一据点箱子中的材料变化
能由原生容器引用直接反映到预览；首次打开建筑菜单时图标即可选择，无需先打开炉子；制作最大数量与
真实可制作数量一致且不会把同一箱子计算两次；制作/建造能消费同公会另一已加载据点的普通箱子材料；
材料不足时不扣除；关闭开关与 LoadMap 后恢复原版行为；食物箱、运输、自动生产和箱子 UI 不共享；
修理明确显示不可用；同一世界内关闭后重新开启时自动重新建立持久登记图；移动、瞄准、打开/关闭菜单
和空闲等待期间目录尝试次数不得增长，日志不得出现联合反复恢复/重建；首次按 B 打开建造菜单应立即
可建造，无需先打开任意制作设施；从菜单进入放置预览、连续放置和提交扣料期间登记边数保持稳定。
新建/拆除/流送普通箱子后只出现一次结构事件驱动的目录校准，新增/删除边按帧预算逐步收敛，不能把
已注入边再次识别为原生来源；关闭、LoadMap 和重新进入存档后所有本 Mod 登记边都被恢复。不要与
IntegratedStorage、UBIM Lite、BlueprintResearch 等修改相同资源路径的 mod 同时测试。

捕获覆盖还应验证：默认关闭且关闭时没有捕获 Hook；开启后普通帕鲁仍可正常捕获，Boss/不可捕获目标
能够进入捕获流程，启用强制成功率后实际捕获成功；解锁与强制两个开关相互独立——仅强制时普通帕鲁
必捕且 Boss/不可捕获目标仍按原版拒绝（含对 NPC 投球仅跳过、无字段写入），只关其一功能不整体失效；
一次投球结束、关闭开关、LoadMap 和热卸载时原字段均被恢复且 Hook 被注销。由于 `SetupInternal` 与
最终捕获判定的实际时序无法由 dump 证明，必须在游戏内确认瞬时事务覆盖窗口确实包含游戏读取点；若
无效，不得仅凭静态构建宣称功能完成。

复活计时移除还应验证：默认关闭且关闭时零写入；开启后终端倒地帕鲁立即复活（PalBoxReviveTime=0）；
关闭开关、切图与热卸载后原值恢复且重读一致；设置实例被世界重建时恢复按"无需恢复"处理而不是报错；
目标暂不可用时等待重试，字段缺失时本世界安全停用。

标记传送还应验证：地图放置至少两个自定义标记后按 F7 传送至水平距离最近的一个（直接落点、无黑屏
过渡）；到达点为标记原始坐标加 ArrivalHeightOffset（默认 0 = 标记地面高度；非零偏移须单独实测）；传送不删除、不修改任何标记（曾实现"传送后自动删除标记"，但地图控件图标 TMap 在活跃 Slate 状态下结构移除多次实测崩溃、收起后重开地图又重建，需求已整体移除；到达标记会霸占最近选择，连续传送需在地图中手动删除或远离该标记）；
地面高度经 LineTraceSingle（通道 0）校正，标记 Z 不可靠、追踪未命中时拒绝传送而不是落图下方；目标区块按世界流送异步加载——远距目标（>100m）首追踪可能命中未加载占位高度，须走"先行到达最佳已知高度 + 1.2s 后静默校正"两段式（近距直接落地；不再空投，常见远距场景第一跳即落地零降落），验证远/近两种距离各一次；每次放置（近距直落/远距空投/贴地校正）后必须调用 SetNoFallDamageHeightLastJumpedLocation 重置下落起点（游戏按 LastJumpedLocation 与落点差值结算坠落伤害，K2_TeleportTo 的角色路径重置对此无效、实测仍受伤）；传送统一用无扫掠 SetActorLocation；引擎拒绝不可达目标时给出提示；骑乘/地牢/战斗门控按配置拦截；无标记、世界未同步时给出对应提示；
LoadMap 后域停用解除；结构不兼容时本世界安全停用。传送原语为 `AActor:K2_SetActorLocation`（bSweep=false 无扫掠精确放置，落点由地面追踪+离地间隙保证）；`K2_TeleportTo` 带路径扫掠，玩家到目标直线穿山时会在阻挡点停下放入地形（实测首次入地、二次正常），不得回退；
`PalSyncTeleportComponent:SyncTeleport` 为有状态序列原语，从 EngineTick 前置
相位调用即使参数/归属/守卫全对齐参考实现仍三次实测内部 -1 崩溃，本 mod 不得回退使用。

还应从桌面连续冷启动游戏多次，确认进入主界面前不会调用技能目录反射导致崩溃；进入存档、Common
主背包就绪后目录应自动加载当前语言名称，手动刷新仍能正常工作。实际帧时间改善必须在游戏内测量。
还应在分别启用捕获覆盖、爪钩覆盖、无限堆叠与资源共享后执行 UE4SS 热重载：卸载线程必须等待下一次
EngineTick 在游戏线程恢复覆盖并注销业务 Hook，随后正常重新加载；日志不得出现非游戏线程
`ProcessEvent`、残留回调、死锁或访问已卸载 DLL。由于 UE4SS 将失效的全局回调闭包交给独立 GC 线程
延迟销毁，而 `CppMod` 会立即 `FreeLibrary`，本 Mod 在构造时固定自身 DLL 到进程退出；热重载只重建
实例与 Hook，不承诺重新映射已替换的 DLL 文件。进程退出路径不以热重载结果替代验证。

## 分支与协作流程

- `main` 仅用于发布；开发基线是 `develop`；
- 所有修改必须从 `develop` 切出独立分支，禁止直接在 `develop` 或 `main` 上提交；分支名前缀：
  `feat/`、`fix/`、`docs/`、`chore/`、`refactor/`、`style/`、`test/`、`diag/`；
- 每个修改集 = 分支 → 推送远端 → PR 到 `develop`；合入 `develop` 验证通过后再按发布流程合入
  `main`；本地 `develop` 与 `main` 只用于 `pull` 对齐，不直接提交。

## C++ 编码规范与 doxygen 注释

以 `.clang-format`、`.clang-tidy`、`.editorconfig` 为准（行尾统一 LF、UTF-8、4 空格缩进），手工
编辑后必须与格式对齐：

- `.clang-format`（Google 风格改编）：C++23、4 空格缩进、列宽 100、大括号 Attach、指针对齐
  Left、`IncludeBlocks: Regroup`（std 头 → 尖括号头 → 引号头），`FixNamespaceComments: true`。
  `format` / `format-check` target 处理 `mods/` 下 Git 跟踪的 C/C++ 文件；
- `.clang-tidy`：启用 bugprone / cert / clang-analyzer / concurrency / cppcoreguidelines /
  misc / modernize / performance / portability / readability 各系列；UE4SS/Unreal 互操作相关
  检查（C 风格转换、`reinterpret_cast`、非 const 全局、成员初始化等）属于刻意豁免。
  `tidy`（写修复）与 `tidy-check`（只读）target 运行于 `compile_commands.json` 中 `mods/` 下的
  翻译单元；`-DPALWORLD_CLANG_TIDY_WARNINGS_AS_ERRORS=ON` 可让 `tidy-check` 遇诊断失败；
- doxygen 注释：公共头文件的类型与函数必须有 `@file` + `@brief`（需要时补 `@details`）；参数用
  `@param[in]` / `@param[out]`，返回值用 `@retval` / `@return`，行为约束与前提条件用 `@note`；
  实现文件关键路径同样写简短注释；
- 命名：类型 PascalCase；函数/变量/成员 snake_case（成员尾部 `_`）；编译期常量 `k` 前缀；
  纯查询函数标记 `[[nodiscard]]`；
- 分层约束：纯值领域层只依赖标准库（`<chrono>`、`<optional>`、`<string>` 等），不包含任何 Unreal
  头；Unreal 类型只在游戏线程适配层出现。

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
  使用对应 Property getter；动态数组使用 `FScriptArrayHelper_InContainer`，不得把参数缓冲区强转为
  `TArray<T>`。读取数组前验证 `Num()` 非负且不超过领域上限，完成范围校验后才能 `reserve`、遍历或做
  窄化转换；
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

## 项目架构

**Super-build 布局。** 根目录的 `CMakeLists.txt` 只做两件事：`add_subdirectory(RE-UE4SS)`（由
`scripts/setup.ps1` 克隆、已被 gitignore）和 `add_subdirectory(mods)`。RE-UE4SS 定义了 `UE4SS`
静态库 target，它会传递性地提供 mod 所需的全部头文件、编译选项和宏定义——包括 C++23 语言标准。
`mods/` 下每个 mod 都是一个链接 `UE4SS` 的 `SHARED` 库。当前 target 位于
`mods/PalworldEditor/CMakeLists.txt`；要新增其他 mod，可复制该目录、修改 target 和运行时元数据，
再在 `mods/CMakeLists.txt` 中增加 `add_subdirectory()`。

**UE4SS 的三元组（triplet）构建系统 + Ninja。** RE-UE4SS 定义了它自己的构建 "triplet"
（`Game__Shipping__Win64`、`CasePreserving__Dev__Win64`……），由它们驱动编译宏定义（`UE_GAME`、
`UE_BUILD_SHIPPING`、`PLATFORM_WINDOWS` 等）的组合——只有当 `$<CONFIG>` 等于某个 triplet 时这些
宏才会生效。Ninja 是单配置（single-config）生成器，所以 preset **显式设置**
`CMAKE_BUILD_TYPE=Game__Shipping__Win64`（即 UE4SS 的默认值）。**必须显式设置**：RE-UE4SS 拉取的
imgui 依赖里，其 examples 含有 `if(NOT CMAKE_BUILD_TYPE) set(CMAKE_BUILD_TYPE Debug ... FORCE)`；
若不显式指定，这个默认就会"赢"，使 `$<CONFIG>` 变成 `Debug` 而不匹配任何 triplet，UE4SS 的关键
宏不会被定义，编译会失败。这就是输出 DLL 落在 `build/Game__Shipping__Win64/bin/` 的原因。

**Mod 入口点契约**（`mods/PalworldEditor/src/dllmain.cpp`）：`PalworldEditorMod` 继承
`RC::CppUserModBase`，设置元数据并重写 `on_update`、`on_unreal_init`；`on_update()` 保持为空，
`on_unreal_init()` 注册 EngineTick 与 LoadMap 前/后回调；DLL 导出 `start_mod()`（构造实例）和
`uninstall_mod()`（销毁实例）。日志用 `RC::Output::send<LogLevel::Verbose>(STR("...{ }...\n"))`
（底层是 `std::format`；`STR()` 会选择正确的字符宽度）。全局 EngineTick/LoadMap Hook 只在
`on_unreal_init()` 注册；资源 UFunction Hook 仅在共享开启且世界可访问时按需解析并注册，关闭共享
或 LoadMap 前立即注销。对象查找、反射读写和 `ProcessEvent` 只允许在 EngineTick/对应 UFunction
游戏线程回调内执行。

**三层分层。** ① 纯值领域层（`inc/` 下 `*_editor.hpp`、`*_service.hpp`、`*_catalog.hpp`、
`*_state.hpp` 等）：只依赖标准库，承载值、快照、请求、队列、校验、账本、帧预算等全部决策逻辑；
② 游戏线程适配层（`src/` 下 `pal_*.cpp`、`*_gateway.cpp`）：在 EngineTick 或对应 UFunction 回调
内做反射查找与读写；③ UI 层（`src/editor_ui.cpp` 与 `src/*_ui.cpp`）：ImGui 回调只做展示与输入。

**线程模型。** ImGui 回调与游戏线程之间只传递标准库快照、互斥锁保护的请求参数和原子请求标志。
所有 UObject 指针都视为非拥有句柄；跨帧状态不得持有 UObject 指针或 Unreal 数组地址，解析得到的
`UObject*` 只在当次回调内使用。

## 针对幻兽帕鲁游戏 mod 的架构及契约

### 业务模块

- `inc/game/pal_game.hpp`：背包、物品和帕鲁 UObject 反射访问；
- `inc/items/item_catalog.hpp`：本地化物品标签、搜索、去重和索引；
- `inc/skills/`：主动/被动技能目录、被动技能分类规则、编辑服务、四词条预设、显式目标锁定与世界代次
  状态；`active_skill_definitions.hpp` 由 `scripts/generate-active-skill-definitions.ps1` 从
  Palworld 1.0 UHT dump 生成，不读取运行时 `UEnum` 内存布局，更新 Palworld/UHT dump 后必须重新
  运行生成脚本；
- `inc/pal_stats/pal_stat_editor.hpp` + `src/pal_stats.cpp`：属性编辑领域与 `SaveParameter`/原生
  setter 的事务适配；
- `inc/pal_identity/` + `src/pal_identity.cpp`：Alpha、Lucky、觉醒三维形态编辑；
- `inc/pal_revive/` + `src/pal_revive.cpp`：队伍帕鲁复活的反射适配与结果分类；
- `inc/grappling_hook/` + `src/grapple_cooldown_gateway.cpp`：爪钩冷却覆盖与恢复；
- `inc/capture_override/` + `src/capture_override/`：投球期间捕获限制的瞬时覆盖、恢复与 Hook 生命周期；
- `inc/revive_timer/` + `src/revive_timer/`：终端复活计时移除的单字段可逆覆盖与恢复账本；
- `inc/waypoint_teleport/` + `src/waypoint_teleport/`：传送至最近自定义地图标记（CustomMarkers 读取
  + 最近标记纯值选择 + K2_SetActorLocation 无扫掠放置）；
- `inc/pal_remote_palbox/remote_palbox.hpp` + `src/pal_remote_palbox/`：远程终端纯值层（按键上升沿
  状态机 300ms 防连点、基地选择策略）与游戏线程运行时；
- `inc/base_resource_sharing/` + `src/pal_base_resources.*`、`src/pal_base_resource_runtime.*`：
  同公会跨据点制作/建造材料共享；
- `inc/common/` + `src/common/`：多个模块实际复用的反射参数 RAII、签名判断和 Hook 登记原语；
- `src/dllmain.cpp`：mod 生命周期、ImGui 和线程间请求交接；
- `src/*_ui.cpp`：各业务模块的 ImGui 界面。

### 反射与目标锁定契约

当前技能目标从唯一属于本地控制器的队伍 Holder 解析，用户点击"选择当前帕鲁"后以
`FPalInstanceID.InstanceId` 和目标代数锁定；只有再次点击该按钮才会切换编辑目标。数字键切换不自动
清空选择；空闲时无论是否已确认目标都不执行当前帕鲁解析。选择和编辑请求会在同一 EngineTick 立即
解析，编辑消费前仍会重新校验当前 GUID，不一致或瞬时解析失败时拒绝写入。GUI 与游戏线程之间不传递
或缓存 `UObject*`，技能 GUI 快照仅在可观察值变化时发布。LoadMap 前置回调递增世界代次、清空所有
待处理操作并撤销写权限；后置回调只恢复读取和目录刷新。原目标 GUID/名称仅用于显示，进入新世界后
必须再次点击"选择当前帕鲁"，技能编辑请求还必须匹配提交时的世界代次。

主动技能目录不读取运行时 `UEnum` 内存布局，而是使用
`scripts/generate-active-skill-definitions.ps1` 从 Palworld 1.0 UHT dump 生成的数值/Raw ID 表。
主动和被动名称由 `PalUIUtility` 按游戏当前语言查询，`PalPlayerInventoryData` 只作为当帧本地化
世界上下文；上下文暂不可用时目录回退为 Raw ID。两个目录区段分别维护可用状态、错误和旧目录回退，
一类失败不禁用另一类。启动时物品扫描属于初始化工作：进入世界后通过 `PalUtility:GetItemIDManager`
解析 `PalStaticItemDataAsset.StaticItemDataMap` 的完整 Raw ID，主数据尚未就绪时才回退已加载
UObject 扫描；所有 Map 地址和 UObject 指针只在当次 EngineTick 使用；回退目录按世界每 2 秒重试且
最多 15 次，成功后立即停止。技能目录及本地化反射必须等待玩家 Common 主背包容器有效，并且只在
现有 2 秒刷新到期或手动请求时检查该安全门；手动刷新不能绕过安全门。这些初始化工作不是常驻逐帧
解析。

被动技能分类选择器在新增/替换流程中以"类别 + 技能"两级下拉框呈现，分类来源是
`PalPassiveSkillManager:GetSkillData` 的 `Rank` 与 `AddWorldTreePal`：`AddWorldTreePal` 为真判定
传说，否则按 `Rank` 划分负面（<0）、极品（≥4）、稀有（3）和普通。分类只在被动目录成功刷新后由
增量任务驱动，每个 EngineTick 最多读取 8 个 ID 且受 500 微秒软预算约束，并在每次 `ProcessEvent`
后检查时间。单个 `GetSkillData` 返回假只把该技能标为未知（仅出现在"全部"），不终止任务；成功读取
的 `{Raw ID -> 元数据}` 纯值缓存保留到 mod 卸载，手动刷新只重试新 ID 与先前失败 ID。分类完成前
只有"全部"可选；任务的结构性错误若发生在已有可用分类之后，界面保留旧具体类别可用并提示"正在使用
上一次成功分类"。LoadMap 前取消任务、撤销分类写权限但保留成功缓存。中文名与 Raw ID 搜索在所有
类别中生效；切换类别清空已选技能但保留搜索文本。

属性编辑契约：帕鲁之魂直接写入 Rank 字段并调用 `OnRep_SaveParameter` 刷新；浓缩同步
`Rank`/`RankUpExp`，伙伴技能等级由 Rank 派生只读显示。工作适应性面板直接编辑
`GotWorkSuitabilityAddRankList` 中的绝对永久附加值，合计等级由原生基础读数与附加值相加显示，提交
时以相对当前值的有符号差值调用 `SetWorkSuitabilityAddRank` 增量写入；物种原本不具备的方向不能新增。
工作适应性拥有独立安全停用域，不会因验证失败而禁用基础属性。任一字段不可访问或写后重读不一致时
对应领域整笔拒绝或回滚，基础属性、工作适应性与形态的安全停用域彼此独立。

形态修改契约：Alpha 通过原生数据库确认的普通/`BOSS_` CharacterID 配对切换（特殊塔主、团本与捕食
者 Boss 不配对）；Lucky 写入 `IsRarePal`，觉醒写入 `bIsAwakening`，三者可任意组合且不消耗材料。
只允许在目标帕鲁已收回时执行；出战判断以本地 Holder `TryGetSpawnedOtomoHandle` 为准，不使用可能
残留的 Actor 对象。写后重读失败时整笔回滚。

远程终端契约：按键触发采用上升沿状态机 + 300ms 防连点 + 进行中保护；圈内判定读取世界设置
`BaseCampAreaRange`（视觉建造圈，通过 `PalUtility:GetGameSetting` 获取），不使用随据点等级膨胀的
据点模型 `AreaRange` 属性；战斗中禁用读取 `APalCharacter::bIsBattleMode` 属性（`IsInCombat` /
`IsInBattle` 函数名在 Palworld 1.0 不存在）。基地选择策略为纯值函数：优先玩家所在圈，否则最近据点。

### 资源共享契约

跨据点资源共享通过 `PalBaseCampManager:GetBaseCampIds` / `TryGetModel` 读取同公会据点，再从
`PalBaseCampModel.ModuleArray` 的 `PalBaseCampModuleItemStorage.ContainerInfos` 筛选 `Chest` 类型
普通仓储；每个原生来源项都必须通过 `PalMapObjectManager:FindConcreteModel` 解析到已加载
ConcreteModel。该功能不使用全局 `FindAllOf`，不扫描或写入 `ItemSlotArray` / `StackCount`，也没有
物品数量预览缓存。

共享实现是世界代次内持续存在、可逆的公会仓储登记图：对每个同公会目标仓储模块，使用
`OnAvailableConcreteModel_ServerInternal` 登记其他据点已加载的普通箱子；目标据点自己的原生容器
不重复登记。制作和建造都只依赖 Palworld 原生仓储关系，不再修改本地主背包 `InventoryMultiHelper`，
也不再把联合绑定到建筑或制作菜单生命周期。菜单关闭、进入放置预览、连续建造和提交扣除期间不得恢复
或重建联合。

`OnAvailableConcreteModel_ServerInternal` 与 `OnNotAvailableConcreteModel_ServerInternal` 的
pre-hook 在参数仍由引擎持有时读取 ConcreteModel 身份，并只查询由上一次安全目录建立、完成登记/注销
后增量维护的排序纯值索引；不得在 Hook 内遍历模块或 `ContainerInfos`。索引必须区分同公会普通仓储、
同模块非普通仓储和其他公会模块；重复可用/不可用通知与被忽略容器不得触发校准，稳定状态无法安全解析
参数时必须 fail-closed。post-hook 不解引用参数对象。`PalBaseCampModel:OnRep_ModuleArray` 只在
初始化明确等待仓储结构时唤醒，稳定状态下周期性的无变化复制必须忽略。所有 Hook 回调内都不得发现
目录、遍历容器或修改数组。下一次 EngineTick 重新发现一次安全目录、剔除边账本中已注入到目标模块的
容器，再计算期望边与已应用边的最小差量，避免把本 Mod 的注入结果当成原生来源并递归扩张。新增/删除
每帧最多执行 4 条，并受 500 微秒软预算限制；每次 `ProcessEvent` 后检查时间。初始化或校准期间到达
的任意数量结构通知只合并成一次后续校准，不得清空正在收敛的差量；当前差量完成后最多再发现一次目录。
持久联合进入 `ready` 后，空闲 EngineTick 只做常量时间阶段判断，不存在 8 秒或其他周期性校准、定时
扫描、后台线程和逐帧容器遍历。

Hook 清单不得包含 `GetBuildObjectDataArrayForUIDisplay`、`IsExistsMaterialForBuildObject`、
`PalUIInGameMainMenuBuildModel:Setup` / `Dispose`、`PalBuilderComponent:ChangeMode` 或
`PalUserWidget:OnClosed`。可选的 `PalUIBuildModel:OnOpenMenu` 与
`PalUIConvertItemModel:Initialize` 只在初始化等待安全上下文时允许触发一次重试，不建立或恢复联合。
`RequestBuild_ToServer` 与 `StartProduction` 只做当前世界代次和持久联合阶段的常量时间检查，不执行
UObject 查找、目录发现、数组读写或日志。所有必需 Hook 已注册后不得继续 5 秒注册轮询；可选 Hook
缺失不阻止稳定状态。

每条由本 Mod 新增的边只保存目标/来源据点 GUID、容器 GUID、ConcreteModel 所有者 GUID 和目标模块
对象全名。登记前后必须验证目标容器为精确 0→1 且只在原序列尾部追加；异常立即调用原生注销接口回滚。
若回滚无法验证，必须把可能已新增的边纳入账本并安全停用本世界制作和建造共享。注销必须验证精确 1→0
且其他序列不变；ConcreteModel 已卸载时只能删除账本对应的精确数组项并调用 OnRep。恢复失败造成的
本世界安全禁用不能通过切换开关绕过；关闭开关仍必须进入 restoring 阶段尽力清理剩余账本。每次安全
目录发现还必须把已发现目标模块中的实物登记与边账本交叉验证：实物边缺失时删除陈旧账本并按期望差量
补回；目标模块未发现时保留账本恢复责任。空差量校准不得打印 `persistent storage graph prepared`，
避免把无操作事件表现成持续扫描。

暂未加载的普通箱子不阻塞已加载部分：不可用事件删除相应跨据点边，可用事件重新建立。跨帧只保存 GUID、
对象全名、纯值计划和标准库账本，不持有 UObject 或 Unreal 数组地址。首次启用但世界上下文/仓储模块
尚未就绪时，只进入等待状态；后续结构事件或低频菜单就绪事件触发下一次尝试，不得逐帧重试。

本地权限必须满足 `IsServer && !IsDedicatedServer`。关闭开关、LoadMap 前置和卸载都先按账本恢复持久
登记，再注销资源 Hook。制作、建造、修理能力独立显示；修理在 Palworld 1.0.1 中保持不可用。Verbose
日志只记录目录发现和持久图差量准备/恢复耗时。资源共享与爪钩无冷却都是本次游戏进程内的动态开关，
每次 DLL 加载默认关闭，不读取、创建或写入 `config.ini`。不要与 IntegratedStorage、UBIM Lite、
BlueprintResearch 或等价的仓储登记/材料路径 mod 同时启用。热卸载无需用户预先关闭开关；卸载线程
只请求并等待下一次 EngineTick，由游戏线程恢复账本并注销业务 Hook，析构线程自身不得访问 Unreal。

**部署契约。** C++ mod 安装到游戏 `Pal/Binaries/Win64/ue4ss/Mods/<ModName>/dlls/main.dll`（把构建
出的 DLL 改名；用 `<ModName>.dll` 也可以）。启用方式：在 mod 文件夹里放一个空的 `enabled.txt`，
**或**者在 `ue4ss/Mods/mods.txt` 中 `Keybinds` 行的上方加一行 `<ModName> : 1`。`deploy` target
（`cmake/Deploy.cmake`）通过 `$<TARGET_FILE:PalworldEditor>` 自动完成这件事，因此无论当前 triplet
输出目录是哪个，源文件路径都始终正确。

## 工具链（clangd / clang-tidy / clang-format）

`.clangd`、`.clang-tidy`、`.clang-format`、`.editorconfig` 和 `.gitattributes` 负责编辑器内的分析
与格式化；行尾统一为 LF。

clangd 读取 `build/compile_commands.json`（preset 里设置了 `CMAKE_EXPORT_COMPILE_COMMANDS=ON`）。
本工程用 MSVC 构建，所以该文件里记录的是 `cl.exe` 调用，clangd 会自动把它们翻译成自己的 clang-cl
前端——不需要单独的 clang 工具链。修改 `CMakeLists.txt` 或新增源文件后，请重新运行
`cmake --preset ninja-msvc-x64` 来刷新它。

- clang-tidy 通过 `.clang-tidy` 在 clangd 内部运行；target 只选择 `mods/` 下的翻译单元，第三方头
  文件仍会被解析，但 `HeaderFilterRegex: 'mods[/\]'` 会抑制其常规诊断。Windows 下 `tidy-check` 是
  单进程批量执行，解析 RE-UE4SS/Unreal 头文件可能耗时较长；
- 如果 clangd 报 "system header not found" / 找不到 Windows SDK，请允许它查询 MSVC 驱动——例如
  VS Code：`"clangd.arguments": ["--query-driver=C:/Program Files/Microsoft Visual Studio/**/Hostx64/x64/cl.exe"]`。

## 权威参考资料

- 官方模板：https://github.com/UE4SS-RE/UE4SSCPPTemplate
- 创建 C++ mod：https://docs.ue4ss.com/guides/creating-a-c++-mod.html
- 安装 C++ mod：https://docs.ue4ss.com/dev/guides/installing-a-c++-mod.html
- RE-UE4SS（框架 + 构建系统）：https://github.com/UE4SS-RE/RE-UE4SS
- Palworld 1.0 运行时：https://github.com/UE4SS-RE/RE-UE4SS/releases ,
  https://steamcommunity.com/workshop/filedetails/?id=3625223587
- Palworld 资料：https://pwmodding.wiki ,
  https://github.com/KURAMAAA0/PalModding/blob/main/ItemIDs.txt ,
  https://github.com/Okaetsu/PalSchema ,
  https://github.com/deafdudecomputers/PalworldSaveTools
