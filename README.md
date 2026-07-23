# PalworldEditor 1.4.0 — Palworld 物品与帕鲁技能编辑器

一个基于 [UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) 的 C++23 mod，为 Palworld 1.0
提供游戏内物品编辑，以及当前选中帕鲁的主动/被动技能编辑。所有操作都在 UE4SS GUI 的 ImGui
窗口中完成，不依赖游戏 F10 控制台。

## 功能

| 功能 | 说明 |
|---|---|
| **给予物品** | 输入物品 ID + 数量 → 调 `AddItem_ServerInternal` |
| **物品浏览器** | 扫描游戏物品定义与本地化名称 → 按名称/ID 搜索 → 点击填充 Raw ID |
| **背包列表** | 读取主背包 → 显示 `本地化名称 [RawId] ×数量` |
| **修改数量** | 选中物品 → 设置新数量（写 `StackCount`） |
| **帕鲁列表** | 扫描全部 `PalIndividualCharacterParameter` → 标记 `[boxed]`/`[active]` |
| **目标帕鲁** | 自动识别当前查看的帕鲁；无有效查看目标时退回 Scan Pals 手动选择 |
| **被动技能** | 最多 4 个；支持新增、替换、删除 |
| **主动技能** | 编辑 3 个 `EquipWaza` 槽位；支持装备、替换、清空 |
| **技能目录** | 运行时加载全部可分配被动与 `EPalWazaID` 主动技能 |
| **中文搜索** | 物品与技能均显示 `中文名 [RawId]`，可按中文名或原始 ID 搜索 |
| **安全修改** | 游戏线程 FIFO 执行，操作后重读；替换失败时恢复原技能 |
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

# 4. 运行纯 C++ 技能编辑测试
cmake --build --preset ninja-msvc-x64 --target PalworldEditorTests
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

1. 在游戏内打开帕鲁盒子或队伍详情并查看一只帕鲁。窗口顶部显示
   `目标：<帕鲁名>（当前查看）`。
2. 如果没有可用的查看目标，点击 **Scan Pals**，从列表中手动选择；窗口显示
   `（手动选择）`。
3. 被动技能区域：
   - 当前被动逐行显示；
   - 点击“替换”后从可搜索下拉框中选择并确认；
   - 点击“删除”移除已有被动；
   - 未满 4 个时可“新增被动技能”。
4. 主动技能区域固定显示 3 个 `EquipWaza` 槽位：
   - 已装备槽位可替换或清空；
   - 第一个空槽可选择并装备新技能。
5. 点击“刷新技能列表”可重新加载技能目录。

下拉框只在点击确认时提交修改；选择候选本身不会立刻写入游戏。已经拥有/装备的技能会从候选中隐藏。
每次修改都在游戏线程执行并重读实际状态；替换未生效时会尝试恢复完整原状态。

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
│   │   └── pal_game.hpp             物品/背包/帕鲁扫描
│   ├── items/
│   │   └── item_catalog.hpp          物品标签、搜索、去重、排序与索引
│   ├── skills/
│   │   ├── pal_skills.hpp           Palworld 技能目录适配层
│   │   ├── skill_catalog.hpp        可搜索技能目录纯逻辑
│   │   └── skill_editor_service.hpp 编辑校验、重读、回滚与 FIFO 队列
│   └── support/
│       └── text_encoding.hpp        UE 宽字符串到 UTF-8
├── src/
│   ├── dllmain.cpp              Mod 类 + GUI + 游戏线程请求分发 + 目标钩子
│   └── pal_skills.cpp           技能目录与游戏函数实现
└── tests/
    └── skill_editor_tests.cpp   不链接 UE4SS 的 CTest 测试
```

仓库根目录：

```text
├── CMakeLists.txt       Super-build：add_subdirectory(RE-UE4SS) + add_subdirectory(mods)
├── CMakePresets.json    Ninja + MSVC x64 preset
├── cmake/Deploy.cmake   deploy target -> 游戏 Mods 目录
└── scripts/             setup.ps1 / build.ps1 / deploy.ps1
```

## 已知限制

- F10 游戏控制台不可用（Palworld ConsoleManager 签名歧义）；所有操作通过 UE4SS GUI。
- 直接写 `StackCount` 绕过游戏复制/通知逻辑；单机可用，多人不可靠。
- 技能编辑支持单机和房主/本地主机；普通联机客户端不支持。
- 主动技能只修改 `EquipWaza`，不会解锁或修改 `MasteredWaza`，也不编辑伙伴技能。
- 技能数组通过 UE4SS 的真实 `TArray<T>` 读取；仍依赖 Palworld 1.0 的 UFunction 参数布局，
  游戏更新后可能需要同步 UHT 签名。
- 是否持久化由游戏公开函数和存档流程决定；修改后请正常保存，并在重载存档后确认。

## 参考

- [UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) · [创建 C++ mod](https://docs.ue4ss.com/guides/creating-a-c++-mod.html)
- [pwmodding.wiki](https://pwmodding.wiki) · [ItemIDs](https://github.com/KURAMAAA0/PalModding/blob/main/ItemIDs.txt)
