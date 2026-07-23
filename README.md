# MyPalMod — Palworld 物品/帕鲁/词条编辑器

一个基于 [UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) 的 **C++23** mod，为 Palworld 1.0 提供**游戏内物品编辑、帕鲁被动词条修改**功能。通过 UE4SS GUI 的浮动 ImGui 窗口操作，不依赖游戏 F10 控制台（Palworld 上已损坏）。

## 功能

| 功能 | 说明 |
|---|---|
| **给予物品** | 输入物品 ID + 数量 → 调 `AddItem_ServerInternal` |
| **物品浏览器** | 扫描全部游戏物品定义（`UPalStaticItemDataBase`）→ 搜索 → 点击填充 |
| **背包列表** | 读取主背包 → 显示物品 × 数量 |
| **修改数量** | 选中物品 → 设置新数量（写 `StackCount`） |
| **帕鲁列表** | 扫描全部 `PalIndividualCharacterParameter` → 标记 `[boxed]`/`[active]` |
| **当前查看帕鲁** | 游戏内查看帕鲁详情时自动识别（hook `GetPassiveSkillList`）|
| **词条编辑** | AddPassiveSkill / RemovePassiveSkill / Read Passives |
| **类名发现** | 扫描 UObject 直方图（调试用） |

## 前置依赖

- **Visual Studio 2022**（最新）+ *"使用 C++ 的桌面开发"*（MSVC + Ninja）
- **CMake ≥ 3.22**，**Git**
- 游戏内装好 **UE4SS Experimental (Palworld)**（Steam Workshop，含 `MemberVariableLayout.ini`）

## 快速开始

所有命令在 **"x64 Native Tools Command Prompt for VS 2022"**（或 VS Developer PowerShell）中运行。

```powershell
# 1. 克隆 RE-UE4SS + 子模块
pwsh scripts/setup.ps1

# 2. 配置 + 构建
cmake --preset ninja-msvc-x64
cmake --build --preset ninja-msvc-x64
#    -> build/Game__Shipping__Win64/bin/MyPalMod.dll

# 3. 部署到游戏
$env:PALWORLD_INSTALL_DIR = "F:\...\Palworld"  # 游戏安装目录
cmake --build --preset ninja-msvc-x64 --target deploy
#    -> Pal/Binaries/Win64/ue4ss/Mods/MyPalMod/dlls/main.dll + enabled.txt
```

## 使用方法

1. 启动 Palworld，读档进入游戏。
2. 打开 **UE4SS GUI**（与游戏同时出现的独立窗口）。
3. 点击 **MyPalMod** 标签 → 弹出浮动窗口。

### 物品编辑
- **Give items**：输入物品 ID（如 `PalSphere_Tera`）+ 数量 → Give。
- **Item browser**：点 "Scan game items" 扫描全部物品 → 搜索 → 点击自动填入。
- **Refresh inventory** → 列出当前背包 → 选中 → Set count 修改数量。

### 帕鲁词条
- **方式一（推荐）**：在游戏内打开帕鲁盒子 → 查看一只帕鲁 → 浮动窗口自动显示 "Currently Viewed: [物种名]" → 输入词条 SkillId → Add/Remove。
- **方式二**：Scan Pals → 从列表选 → Add/Remove。
- **获取词条 SkillId**：选中帕鲁后点 Read → Console 打印该帕鲁现有的词条 FName（如 `ElementResist_Leaf_1_PAL`）。

## 物品 ID 参考

Bare ID（无前缀），完整列表见 [ItemIDs.txt](https://github.com/KURAMAAA0/PalModding/blob/main/ItemIDs.txt)。常用：`PalSphere`、`PalSphere_Tera`、`PalSphere_Legend`、`Stone`、`Wood`、`Money`、`AncientParts2`。

## 目录结构

```
mods/MyPalMod/
├── src/
│   ├── dllmain.cpp      Mod 类 + GUI + 请求分发 + 钩子
│   ├── pal_game.hpp     游戏交互函数（物品/背包/帕鲁/词条）
│   ├── item_database.h  精选物品 ID 列表
│   └── CMakeLists.txt
├── CMakeLists.txt       Super-build：add_subdirectory(RE-UE4SS) + add_subdirectory(mods)
├── CMakePresets.json    Ninja + MSVC x64 preset
├── cmake/Deploy.cmake   deploy target -> 游戏 Mods 目录
└── scripts/             setup.ps1 / build.ps1 / deploy.ps1
```

## 已知限制

- F10 游戏控制台不可用（Palworld ConsoleManager 签名歧义）；所有操作通过 UE4SS GUI。
- 直接写 `StackCount` 绕过游戏复制/通知逻辑；单机可用，多人不可靠。
- `ProcessEvent` 参数布局基于手动声明的结构体；游戏更新后可能需要修正。
- `GetPalAssignablePassiveIDs`（批量获取词条）会导致内存损坏/崩溃，已禁用；用 Read Passives 替代。

## 参考

- [UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) · [创建 C++ mod](https://docs.ue4ss.com/guides/creating-a-c++-mod.html)
- [pwmodding.wiki](https://pwmodding.wiki) · [ItemIDs](https://github.com/KURAMAAA0/PalModding/blob/main/ItemIDs.txt)
