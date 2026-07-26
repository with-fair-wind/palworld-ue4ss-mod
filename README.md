# PalworldEditor 1.6.0 — Palworld 物品、帕鲁技能与据点资源编辑器

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
| **被动技能** | 最多 4 个；支持新增、替换、删除 |
| **主动技能** | 编辑 3 个 `EquipWaza` 槽位；支持装备、替换、清空 |
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
   - 当前被动逐行显示；
   - 点击“替换”后从可搜索下拉框中选择并确认；
   - 点击“删除”移除已有被动；
   - 未满 4 个时可“新增被动技能”。
4. 主动技能区域固定显示 3 个 `EquipWaza` 槽位：
   - 已装备槽位可替换或清空；
   - 第一个空槽可选择并装备新技能。
5. 进入存档且 Common 主背包容器就绪后，mod 会每两秒自动重试一次完整技能目录，成功后停止重试；点击
   “刷新技能列表”会跳过时间节流，但同样必须先通过运行时安全检查，随后重新加载主动技能、被动技能及
   当前游戏语言名称。

下拉框只在点击确认时提交修改；选择候选本身不会立刻写入游戏。已经拥有/装备的技能会从候选中隐藏。
每次修改都在游戏线程执行并重读实际状态；替换未生效时会尝试恢复完整原状态。
点击“选择当前帕鲁”后，目标保持锁定；数字键切换高亮或单帧解析失败不会自动替换、清空该选择。
当前高亮目标与锁定 GUID 不同时，修改按钮会暂停，必须再次点击“选择当前帕鲁”才会切换编辑目标。
在尚未确认目标且没有选择/修改请求时，mod 不解析当前帕鲁；确认后最多每 250 毫秒后台校验一次高亮目标。
点击“选择当前帕鲁”或提交技能修改会跳过该间隔，在同一游戏线程回调中立即重新解析并校验 GUID，因此
250 毫秒仅影响 GUI 发现数字键切换的延迟，不会放宽写入安全检查。解析得到的 `UObject*` 只在当前回调中使用。
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

游戏内验证时可在场景中保留一只野生帕鲁，同时让队伍 UI 高亮另一只队伍帕鲁。点击
“选择当前帕鲁”后，目标必须是下一次按 E 会召唤的队伍帕鲁，而不是场景中的野生帕鲁。

### 同公会跨据点资源共享

1. 展开“据点资源共享”，勾选“同公会跨据点资源共享”。默认值为关闭，选择会写入
   `ue4ss/Mods/PalworldEditor/config.ini`。
2. 进入单人世界或由本机担任房主的世界；状态区会显示已加载的同公会据点和普通资源容器数量。
3. 在据点 A 发起制作或建造时，可使用据点 B 普通箱子中的材料。箱子界面仍只显示本地箱子，
   物品不会预先移动，实际扣除、复制和存档仍由 Palworld 完成。
4. 当前据点/当前资源助手已有的容器保持在队列前部，因此优先遵循原版本地消耗顺序。
5. 关闭开关或切换世界时，mod 会先恢复并验证所有临时容器引用，再恢复原版行为。

关闭共享时不会注册任何资源 UFunction Hook，也不会发现据点容器。开启后，mod 通过
`PalBaseCampManager`、`PalBaseCampModel`、`PalBaseCampModuleItemStorage` 和 `PalMapObjectManager`
直接取得同公会普通箱子，不使用全局 `FindAllOf`，也不扫描 `ItemSlotArray` 或统计每个槽位数量。
据点/箱子结构事件会立即使目录失效，并以 8 秒低频校准作为事件遗漏时的安全兜底。

进入建造模式或打开制作界面时，mod 才建立短生命周期资源联合；会话期间 Palworld 原生的预览、校验和扣料
都看到同一组容器。退出建造模式或制作界面空闲 1.5 秒后会按记录的注入次数恢复引用。跨帧只保存 GUID、
对象全名和序列账本，不保存 `UObject*` 或 Unreal 数组地址。UE4SS Verbose 日志只记录目录校准、联合建立和
恢复耗时，不会逐帧或逐次预览刷屏。

此功能只处理制作与建造材料，不共享食物箱、帕鲁运输、自动生产或箱子 UI。修理材料共享目前明确显示
“不可用”，因为 Palworld 1.0.1 尚未验证安全的修理检查与扣除入口。不要同时启用 IntegratedStorage、
UBIM Lite、BlueprintResearch 或其他会修改相同制作/建造资源路径的 mod。

配置文件只接受：

```ini
[BaseResourceSharing]
Enabled=true
```

也可把值设为 `false`。配置缺失或无效时安全回退为关闭。

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
│   │   ├── resource_session.hpp      目录校准调度与制作/建造会话租约
│   │   └── settings.hpp              默认关闭的持久化配置
│   ├── items/
│   │   └── item_catalog.hpp          物品标签、搜索、去重、排序与索引
│   ├── skills/
│   │   ├── active_skill_definitions.hpp Palworld 1.0 主动技能数值/Raw ID 生成表
│   │   ├── pal_resolution_scheduler.hpp 当前帕鲁按需解析与 250 ms 校验调度
│   │   ├── pal_skills.hpp           Palworld 技能目录适配层
│   │   ├── selected_target_state.hpp 当前目标状态与过期请求保护
│   │   ├── skill_catalog.hpp        可搜索技能目录纯逻辑
│   │   ├── skill_editor_service.hpp 编辑校验、重读、回滚与 FIFO 队列
│   │   └── world_session_state.hpp  LoadMap 世界代次与重新确认状态
│   └── support/
│       └── text_encoding.hpp        UE 宽字符串到 UTF-8
├── src/
│   ├── base_resource_settings.cpp 配置解析与原子保存
│   ├── dllmain.cpp                Mod 类 + GUI + EngineTick/LoadMap 请求分发
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
