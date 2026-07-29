# PalworldEditor 1.6.9 — Palworld 物品、帕鲁技能与据点资源编辑器

一个基于 [UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) 的 C++23 mod，为 Palworld 1.0
提供游戏内物品编辑、当前选中帕鲁的主动/被动技能编辑，以及可选的同公会跨据点制作/建造材料共享。
所有操作都在 UE4SS GUI 的 ImGui 窗口中完成，不依赖游戏 F10 控制台。

## 功能

| 功能 | 说明 |
|---|---|
| **给予物品** | 输入物品 ID + 数量 → 调 `AddItem_ServerInternal` |
| **物品浏览器** | 扫描游戏物品定义与本地化名称 → 按名称/ID 搜索 → 点击填充 Raw ID |
| **背包列表** | 读取主背包 → 显示 `本地化名称 [RawId] ×数量` |
| **修改数量** | 选中物品 → 设置新数量（写 `StackCount`） |
| **目标帕鲁** | 用数字键高亮下一次按 E 会召唤的队伍帕鲁，再点击“选择当前帕鲁”确认；确认前不做后台扫描 |
| **被动技能** | 最多 4 个；新增/替换前先按 普通/稀有/极品/传说/负面 分类筛选，支持新增、替换、删除与四词条预设 |
| **主动技能** | 编辑 3 个 `EquipWaza` 槽位；支持装备、替换、清空 |
| **属性修改** | 选中帕鲁后修改等级（1–80）、普通个体值 HP/攻击/防御（0–100）与亲密度（0–10）；只提交差量并做写后验证 |
| **爪钩枪无冷却** | 默认关闭；仅覆盖可明确识别的爪钩对象，并在关闭和切图时恢复各自原值；热卸载前应先关闭 |
| **技能目录** | 进入存档且主背包就绪后自动重试完整目录；手动刷新也必须通过运行时安全检查 |
| **本地化搜索** | 物品与技能均显示 `当前语言名称 [RawId]`，可按名称或原始 ID 搜索 |
| **据点资源共享** | 默认关闭；单人/本地房主可让制作和建造使用同公会所有已加载普通仓储材料 |
| **安全修改** | Unreal 访问只在 EngineTick 游戏线程执行；修改前立即重查 GUID + 目标/世界代次，跨世界请求自动失效 |
| **类名发现** | 扫描 UObject 直方图（调试用） |

## 前置依赖

- **Visual Studio 2022**（最新）+ *"使用 C++ 的桌面开发"*（MSVC + Ninja）
- **CMake ≥ 3.22**，**Git**
- **Rust stable**（`cargo` + `rustc`；当前 RE-UE4SS 会构建 Rust 实现的 PatternSleuth 依赖）
- 游戏内装好 **UE4SS Experimental (Palworld)**（Steam Workshop，含 `MemberVariableLayout.ini`）

## 快速开始

所有命令在 **"x64 Native Tools Command Prompt for VS 2022"**（或 VS Developer PowerShell）中运行。

```powershell
# 1. 克隆 RE-UE4SS + 子模块
pwsh scripts/setup.ps1

# 2. 在首次配置前设置部署目标（不需要部署时可省略）
$env:PALWORLD_INSTALL_DIR = "F:\...\Palworld"  # 游戏安装目录

# 3. 配置 + 构建
cmake --preset ninja-msvc-x64
cmake --build --preset ninja-msvc-x64 --target PalworldEditor
#    -> build/Game__Shipping__Win64/bin/PalworldEditor.dll

# 4. 运行纯 C++ 技能编辑与据点资源安全契约测试
cmake --build --preset ninja-msvc-x64 --target PalworldEditorTests PalworldEditorBaseResourceSharingTests
ctest --test-dir build --output-on-failure

# 5. 部署到游戏
cmake --build --preset ninja-msvc-x64 --target deploy
#    -> Pal/Binaries/Win64/ue4ss/Mods/PalworldEditor/dlls/main.dll + enabled.txt
```

如果在配置完成后才设置或修改 `PALWORLD_INSTALL_DIR`，请重新运行
`cmake --preset ninja-msvc-x64`，让部署 target 刷新缓存路径。

## 代码质量工具

clangd 会自动读取根目录的 `.clang-format`、`.clang-tidy` 和
`build/compile_commands.json`。CMake 还会在 PATH 中找到相应 LLVM 工具时注册以下手动 target；
它们不会随普通构建自动执行，也不会处理 `RE-UE4SS/`：

```powershell
# 按 Google 风格格式化 mods/ 下 Git 跟踪的 C/C++ 文件
cmake --build --preset ninja-msvc-x64 --target format

# 只检查格式，不修改文件
cmake --build --preset ninja-msvc-x64 --target format-check

# 对 compile_commands.json 中 mods/ 下的翻译单元应用 clang-tidy 修复
cmake --build --preset ninja-msvc-x64 --target tidy

# 只读静态检查；默认不会把 warning 升级为 error
cmake --build --preset ninja-msvc-x64 --target tidy-check
```

如需让 `tidy-check` 遇到任意诊断时失败，配置时增加
`-DPALWORLD_CLANG_TIDY_WARNINGS_AS_ERRORS=ON`。修改 CMake 或新增源文件后，应重新运行
`cmake --preset ninja-msvc-x64` 刷新 `build/compile_commands.json`。

## 使用方法

1. 启动 Palworld，读档进入游戏。
2. 打开 **UE4SS GUI**（与游戏同时出现的独立窗口）。
3. 点击 **PalworldEditor** 标签 → 弹出浮动窗口。

### 物品编辑
- **Give items**：输入物品 ID（如 `PalSphere_Tera`）+ 数量 → Give。
- **Item browser**：进入游戏后会自动扫描一次当前已加载的物品定义与当前语言名称；也可点
  "Scan game items" 重新扫描。列表显示 `名称 [RawId]`，支持按名称或 ID 搜索；点击后只把 Raw ID
  填入 Give 输入框。
- **Refresh inventory** → 以 `名称 [RawId] ×数量` 列出当前背包 → 选中 → Set count 修改数量。

### 帕鲁主动/被动技能

1. 确保队伍中有帕鲁，并用数字键高亮下一次按 E 会召唤的队伍帕鲁；无需打开帕鲁盒子或详情页。
2. 点击“选择当前帕鲁”。成功后窗口显示 `当前已选择帕鲁：<CharacterID>` 和技能编辑区。
   点击按钮前不会读取或修改任何帕鲁技能。
3. 被动技能区域：
   - 从“词条预设”下拉框选择“工作毕业1”或“工作毕业2”，确认四个当前语言词条名称后点击“应用预设”；
   - 选择预设本身不会修改技能；“应用预设”会把完整被动集合精确替换成该预设；
   - 当前被动逐行显示，并按类别着色（普通白、稀有黄、极品蓝、传说紫、负面红）；
   - 点击“替换”或“新增被动技能”后，先在“类别”下拉框选择 全部/普通/稀有/极品/传说/负面，再从可搜索下拉框中选择并确认；
   - 类别与搜索是持久筛选条件：切换类别会清空已选技能但保留搜索文本，搜索同时匹配中文名与 Raw ID；
   - 点击“删除”移除已有被动。
4. 主动技能区域固定显示 3 个 `EquipWaza` 槽位：
   - 已装备槽位可替换或清空；
   - 第一个空槽可选择并装备新技能。
5. 进入存档且 Common 主背包容器就绪后，mod 会每两秒自动重试一次完整技能目录，成功后停止重试；点击
   “刷新技能列表”会跳过时间节流，但同样必须先通过运行时安全检查，随后重新加载主动技能、被动技能及
   当前游戏语言名称。

下拉框只在点击确认时提交修改；选择候选本身不会立刻写入游戏。已经拥有/装备的技能会从候选中隐藏。
每次修改都在游戏线程执行并重读实际状态；替换未生效时会尝试恢复完整原状态。应用预设只提交一个请求，
仅写入当前词条与目标预设之间的差异；目标已经一致时不写入，因游戏拒绝而部分生效时会按重读状态回滚。
预设是 `inc/skills/passive_skill_presets.hpp` 中的编译期 C++ 表，新增预设不需要增加 Hook、扫描或逐帧任务。
点击“选择当前帕鲁”后，目标保持为纯值锁定；数字键切换高亮不会自动替换或清空该选择，只有再次点击该按钮
才会切换编辑目标。空闲时无论是否已确认目标，mod 都不再后台解析队伍 Holder。
点击“选择当前帕鲁”或提交技能修改时，mod 会在同一游戏线程回调中立即重新解析并校验 GUID；如果数字键
高亮目标已变化，本次修改会在写入前被拒绝。解析得到的 `UObject*` 只在当前回调中使用。
退出世界、切换地图或重新进入存档时，LoadMap 回调会清空所有待处理物品/技能操作并撤销技能写权限；
原帕鲁名称和 GUID 仅保留用于显示。进入新世界后必须再次点击“选择当前帕鲁”，旧世界请求不会补执行。
目标从唯一属于本地控制器的队伍 Holder 解析，并以
`FPalInstanceID.InstanceId` 区分同种帕鲁；GUI 与游戏线程之间不传递或缓存 `UObject*`。
主动技能数值与 Raw ID 来自生成的 Palworld 1.0 定义表，不读取运行时 `UEnum` 内存布局；名称由
`PalUIUtility` 按游戏当前语言查询，并使用 `PalPlayerInventoryData` 作为当帧世界上下文。游戏运行时尚未
完整就绪时目录可回退显示 Raw ID，但所有技能写入保持禁用；自动重试或手动刷新成功后恢复当前语言名称和编辑。
主动和被动目录分别保留可用状态和最近错误，一类刷新失败不会清空另一类已经可用的目录。
启动时的一次物品扫描保持不变；技能目录及本地化反射会等待 Common 主背包容器有效，并且只在现有 2 秒刷新
到期或手动请求时检查，不会在启动画面或每个 EngineTick 强行查询尚未就绪的玩家运行时。
主动技能候选会排除稳定 Raw ID 中明确属于游戏内部用途的 `Human_`、`_GYM_`、`Raid` 和 `Boss`
条目；普通技能、`SelfDestruct` 和正常的 `Unique_*` 帕鲁专属技能仍会保留。

被动技能分类来自 `PalPassiveSkillManager:GetSkillData` 的 `Rank` 与 `AddWorldTreePal`：传说优先，
其次按 `Rank` 划分负面（<0）、极品（≥4）、稀有（3）和普通。分类在被动目录加载完成后以每个 EngineTick
最多 8 个 ID、500 微秒软预算的小批次在游戏线程后台完成；分类完成前只有“全部”可选，已成功读取的分类会缓存到
mod 卸载，手动刷新只重试新增和先前失败的技能。中文名与 Raw ID 搜索在所有类别中都生效。分类读取结构失败但
已有上一次成功分类时，界面会提示“正在使用上一次成功分类”并保留具体类别可用。

游戏内验证时可在场景中保留一只野生帕鲁，同时让队伍 UI 高亮另一只队伍帕鲁。点击
“选择当前帕鲁”后，目标必须是下一次按 E 会召唤的队伍帕鲁，而不是场景中的野生帕鲁。

### 同公会跨据点资源共享

1. 展开“据点资源共享”，勾选“同公会跨据点资源共享”。该开关仅在本次游戏进程有效，每次启动默认关闭，
   不读取、创建或写入 `config.ini`。
2. 进入单人世界或由本机担任房主的世界；状态区会显示已加载的同公会据点和普通资源容器数量。
3. 在据点 A 发起制作或建造时，可使用据点 B 普通箱子中的材料。箱子界面仍只显示本地箱子，
   物品不会预先移动，实际扣除、复制和存档仍由 Palworld 完成。
4. 当前据点/当前资源助手已有的容器保持在队列前部，因此优先遵循原版本地消耗顺序。
5. 关闭开关或切换世界时，mod 会先恢复并验证所有临时容器引用，再恢复原版行为。
6. 在同一世界内重新打开开关会按当前世界代次重置按需目录状态；下次打开制作或建造界面时自动恢复计数。

关闭共享时不会注册任何资源 UFunction Hook，也不会发现据点容器。开启后，mod 通过
`PalBaseCampManager`、`PalBaseCampModel`、`PalBaseCampModuleItemStorage` 和 `PalMapObjectManager`
直接取得同公会普通箱子，不使用全局 `FindAllOf`，也不扫描 `ItemSlotArray` 或统计每个槽位数量。
不注册据点/箱子结构事件、配方预览或建造资格等高频 Hook；不存在 60 秒低频校准、后台指数退避或其他空闲目录
发现。每个新的制作或建造会话只在原版资格计算前同步发现一次目录，同一会话不会重复发现。已登记但尚未加载的普通
箱子不会使整次发现失败：已加载部分立即可用，未加载部分从本次联合排除，并在下一次材料操作开始时重新尝试。
同一世界内重新开启只重置按需目录状态，不增加线程、全局扫描、槽位扫描或逐帧任务；关闭状态保持零资源 Hook
和零目录发现。原生 UFunction 直接挂接；Blueprint UFunction 共用一对可注销的轻量分发回调，每次只比较最多
八个缓存函数指针，不构造函数全名，也不进入 UE4SS 通用 UFunction Hook 的全名散列表分发。

原版建筑列表、材料资格、菜单 `Setup` 和制作模型 `Initialize` 都在原函数执行前建立短生命周期资源联合，使
首次图标/配方资格计算即可读取共享材料。制作只向本地主背包 `InventoryMultiHelper` 注入其他据点的普通箱子，
排除当前据点原版已经能够访问的箱子；建造只向当前据点仓储模块注入其他据点箱子，使预览、实际扣料和取消返还
使用同一个原版入口。制作与建造不能并存，新的前台操作会先恢复旧联合再抢占。写入后会调用原版 `OnRep`、重读
并验证每个注入容器恰好出现一次；数量或顺序异常会回滚并在本世界安全停用对应能力。
`StartProduction` 和 `RequestBuild_ToServer` 在真实提交前只读验证当前据点、世界代次和联合序列。制作界面由
`PalUserWidget:OnClosed` 精确识别 `PalHUDDispatchParameter_ConvertItem` 后释放，退出建造模式时释放建造会话。
建筑菜单 `Setup` 完成后只向当前建造模型发送一次原生 `OnUpdateInventory(Container)` 事件，使首次资格缓存
立即重算；不会重复触发 Helper 的 `OnRep_Containers`，也不会增加逐物品或逐帧 Hook。
跨帧只保存 GUID、对象全名和序列账本，不保存 `UObject*` 或 Unreal 数组地址。资源 Hook 清单只保留八个精确
前台资格、提交和生命周期入口。若首次资格回调早于目录初始化，会在该次
pre-hook 中同步执行一次有界目录 bootstrap 并建立联合，不需要先打开另一座建筑，也不会进入逐帧重试。

只有终端、尚无普通仓储模块的据点仍计入据点数量，但不会贡献材料，也不能作为建造共享目标。每次新前台会话和
真实提交前，mod 通过本地控制器的 `K2_GetPawn`、Pawn 的 `InsideBaseCampCheckComponent` 与
`GetInsideBaseCampModel` 读取游戏原生确认的当前据点；不会按玩家位置调用 `GetNearestBaseCamp` 猜测。
解析失败或当前据点不在同公会普通仓储目录时保持原版行为。界面仅发布纯值诊断：前台操作、单一联合入口、当前
据点确认状态以及最近目录/联合耗时、最近成功目录耗时、本世界峰值、尝试次数和暂未加载容器数，不提供逐帧材料
数量预览。

此功能只处理制作与建造材料，不共享食物箱、帕鲁运输、自动生产或箱子 UI。修理材料共享目前明确显示
“不可用”，因为 Palworld 1.0.1 尚未验证安全的修理检查与扣除入口。不要同时启用 IntegratedStorage、
UBIM Lite、BlueprintResearch 或其他会修改相同制作/建造资源路径的 mod。

## 物品 ID

浏览器通过 UE4SS 运行时读取已经加载的 `PalStaticItemData*` UObject 的 `ID`，并调用
`PalUIUtility:GetItemName` 获取游戏当前语言的名称，不再维护静态物品表，也不需要解包游戏资源。
游戏语言为中文时，物品浏览器和背包显示 `中文名 [RawId]`；若名称或目录尚不可用则回退为 Raw ID。
扫描范围取决于游戏当时已经加载的物品定义；仍可在 Give 输入框中手动输入 Bare ID（无前缀）。

## 目录结构

```
mods/PalworldEditor/
├── CMakeLists.txt
├── inc/
│   ├── game/
│   │   └── pal_game.hpp             物品/背包与当前待出战帕鲁解析
│   ├── base_resource_sharing/
│   │   ├── hook_manifest.hpp         Palworld 1.0.1 Hook 能力清单
│   │   ├── pal_base_resources.hpp    跨据点资源桥值接口
│   │   ├── resource_pool.hpp         过滤、排序、能力与恢复纯逻辑
│   │   └── resource_session.hpp      目录校准调度与单一前台材料会话
│   ├── items/
│   │   └── item_catalog.hpp          物品标签、搜索、去重、排序与索引
│   ├── pal_stats/
│   │   ├── pal_stat_editor.hpp       属性快照、差量草稿、请求槽与验证
│   │   └── pal_stats.hpp             SaveParameter 游戏线程网关
│   ├── grappling_hook/
│   │   ├── cooldown_service.hpp      精确 ID 白名单、一次性工作状态与原值账本
│   │   └── cooldown_gateway.hpp      冷却覆盖/恢复游戏线程网关
│   ├── skills/
│   │   ├── active_skill_definitions.hpp Palworld 1.0 主动技能数值/Raw ID 生成表
│   │   ├── pal_resolution_scheduler.hpp 当前帕鲁选择/编辑事件决策与纯值解析快照
│   │   ├── pal_skills.hpp           Palworld 技能目录适配层
│   │   ├── selected_target_state.hpp 当前目标状态与过期请求保护
│   │   ├── skill_catalog.hpp        可搜索技能目录纯逻辑
│   │   ├── skill_editor_service.hpp 编辑校验、重读、回滚与 FIFO 队列
│   │   └── world_session_state.hpp  LoadMap 世界代次与重新确认状态
│   └── support/
│       └── text_encoding.hpp        UE 宽字符串到 UTF-8
├── src/
│   ├── dllmain.cpp                Mod 类 + GUI + EngineTick/LoadMap 请求分发
│   ├── grapple_cooldown_gateway.cpp 精确识别、覆盖、验证与恢复爪钩冷却
│   ├── pal_base_resource_runtime.* 管理器发现与可逆临时联合
│   ├── pal_base_resources.cpp     事件调度、会话租约与 Hook
│   └── pal_skills.cpp             技能目录与游戏函数实现
└── tests/
    ├── base_resource_sharing_tests.cpp 资源共享纯 C++ 安全契约
    └── skill_editor_tests.cpp          技能编辑纯 C++ 测试
```

仓库根目录：

```text
├── CMakeLists.txt       Super-build：add_subdirectory(RE-UE4SS) + add_subdirectory(mods)
├── CMakePresets.json    Ninja + MSVC x64 preset
├── cmake/Deploy.cmake   deploy target -> 游戏 Mods 目录
└── scripts/             setup/build/deploy + 主动技能定义生成脚本
```

## 已知限制

- F10 游戏控制台不可用（Palworld ConsoleManager 签名歧义）；所有操作通过 UE4SS GUI。
- 直接写 `StackCount` 绕过游戏复制/通知逻辑；单机可用，多人不可靠。
- 技能编辑支持单机和房主/本地主机；普通联机客户端不支持。
- 据点资源共享同样只支持单人世界/本地房主，且只包含当前已加载的同公会普通仓储容器。
- 修理材料共享尚不可用；缺少必需 Hook、容器解析不完整或恢复序列不一致时，对应能力会按世界失败关闭。
- 1.6.8 将爪钩与资源共享改为进程内动态开关，并把制作/建造收敛为互斥使用同一个玩家材料 Helper；
  首次资格计算前完成有界 bootstrap，联合写后验证恰好一次，制作界面关闭时事件驱动恢复，异常时回滚并按
  世界安全停用。
- 1.6.9 后续修订移除目录空闲兜底与后台重试，将目录发现限制在每个新材料会话一次；制作排除当前据点容器，
  建造改用当前据点仓储模块，真实提交前验证联合，并通过建造模型单次库存更新修复首次资格缓存。
- 爪钩功能只在切换开关或新世界重应用时扫描一次；关闭和 LoadMap 前会在游戏线程恢复原值。若使用 UE4SS
  热卸载 C++ mod，请先在界面关闭该功能，析构阶段不会访问 Unreal。
- 不要与 UBIM Lite 或其他修改相同制作/建造资源请求路径的 mod 同时运行。
- 主动技能只修改 `EquipWaza`，不会解锁或修改 `MasteredWaza`，也不编辑伙伴技能。
- 技能数组通过 UE4SS 的真实 `TArray<T>` 读取；仍依赖 Palworld 1.0 的 UFunction 参数布局，
  游戏更新后可能需要同步 UHT 签名。
- 游戏版本更新并替换 `UHTHeaderDump/` 后，需要运行
  `powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/generate-active-skill-definitions.ps1`
  更新主动技能定义表。
- 是否持久化由游戏公开函数和存档流程决定；修改后请正常保存，并在重载存档后确认。

## 参考

- [UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) · [创建 C++ mod](https://docs.ue4ss.com/guides/creating-a-c++-mod.html)
- [pwmodding.wiki](https://pwmodding.wiki) · [ItemIDs](https://github.com/KURAMAAA0/PalModding/blob/main/ItemIDs.txt)
- [PalSchema](https://github.com/Okaetsu/PalSchema) · [PalworldSaveTools](https://github.com/deafdudecomputers/PalworldSaveTools)
