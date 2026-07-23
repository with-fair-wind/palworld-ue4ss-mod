# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

> 本文件为 Claude Code 在本仓库中工作时提供指引。

## 这是什么

一个 **UE4SS C++ mod**（C++23 / CMake / Ninja）面向 **Palworld 1.0**。它是一个**游戏内物品/帕鲁/词条编辑器**，通过 UE4SS GUI 的浮动 ImGui 窗口操作。构建产物是 `MyPalMod.dll`（v1.3.1）。

mod DLL 在构建期**硬编码了 Palworld API**（`/Script/Pal.*` 函数路径、`PalPlayerInventoryData`、`PalIndividualCharacterParameter` 等），不再与具体游戏无关——构建脚手架（CMake/RE-UE4SS 链接）仍然通用，但 mod 本体专门针对 Palworld。

Palworld 1.0 需要 UE4SS **experimental** 运行时（Steam Workshop "UE4SS Experimental (Palworld)" + PalSchema，含 `MemberVariableLayout.ini`）。F10 控制台在 Palworld 上不可用（ConsoleManager 签名歧义），因此所有交互通过 UE4SS GUI 窗口进行。

## 前置依赖

- **Visual Studio 2022**（最新版），*"使用 C++ 的桌面开发"* 工作负载（MSVC + Ninja）。
- **CMake ≥ 3.22**，**Git**。
- **Rust 不需要**（preset 设 `UE4SS_VERSION_CHECK=OFF`；只构建 mod target，不构建 UVTD）。
- 所有构建命令必须在 **MSVC 环境**中运行。

## 常用命令

```powershell
pwsh scripts/setup.ps1                          # 首次：克隆 RE-UE4SS + 子模块
$env:PALWORLD_INSTALL_DIR = "F:\...\Palworld"   # 部署目标（可选）
cmake --preset ninja-msvc-x64                    # 配置（MSVC dev shell）
cmake --build --preset ninja-msvc-x64            # 构建 -> build/Game__Shipping__Win64/bin/MyPalMod.dll
cmake --build --preset ninja-msvc-x64 --target deploy  # 部署到游戏
```

## 架构

### 文件结构

```
mods/MyPalMod/src/
├── dllmain.cpp     (510 行) — MyPalMod 类：GUI 渲染、on_update 请求分发、ProcessEvent 钩子
├── pal_game.hpp    (403 行) — 游戏交互函数（namespace pal_game）：物品/背包/帕鲁/词条/发现
├── item_database.h (93 行)  — 精选物品 ID 列表（物品浏览器的兜底数据）
└── CMakeLists.txt
```

### 线程模型

```
ImGui GUI 线程                    游戏线程 (on_update)
─────────────                    ──────────────────
register_tab 回调                 on_update() 每帧调用
  ↓                               ↓
  读/写 UI 状态                   读请求标志 (atomic<bool>)
  写 atomic 标志 (want_*)     →   执行 pal_game::* 函数
  写 mutex 保护参数 (req_mutex_)  写 mutex 保护缓存 (inv_mutex_)
  读 mutex 保护缓存 (inv_mutex_)  ← 结果回流到 UI
```

所有 UE 反射调用（ProcessEvent、GetPropertyByNameInChain、ForEachUObject）**只在游戏线程**执行。ImGui 线程只做 UI + 标志/缓存读写。

### 关键 API（从 UHTHeaderDump 发现）

```
物品：  UPalPlayerInventoryData::AddItem_ServerInternal(FName, int32, bool, float, bool) -> enum
        UPalPlayerInventoryData::TryGetContainerFromInventoryType(uint8, UPalItemContainer*&)
        UPalItemContainer::Num() / Get(i) -> UPalItemSlot { StackCount:int32, ItemId.StaticId:FName }
帕鲁：  UPalIndividualCharacterParameter::AddPassiveSkill(FName, FName)
        UPalIndividualCharacterParameter::RemovePassiveSkill(FName)
        UPalIndividualCharacterParameter:GetPassiveSkillList -> TArray<FName>
钩子：  Hook::RegisterProcessEventPreCallback — 监听 GetPassiveSkillList 调用以追踪"当前查看的帕鲁"
GUI：   CppUserModBase::register_tab + UE4SS_ENABLE_IMGUI + ImGui::Begin (浮动窗口)
```

### Mod 入口点契约

`start_mod()` 构造 `MyPalMod`，`uninstall_mod()` 析构。生命周期钩子：
- `MyPalMod()` — 设置元数据 + `register_tab`（注册 GUI 标签）
- `on_unreal_init()` — 注册 ProcessEvent 钩子（PalViewTracker）
- `on_update()` — 每帧：检查请求标志 → 执行游戏函数 → 更新缓存

## 工具链（clangd / clang-tidy / clang-format）

`.clangd`、`.clang-tidy`、`.clang-format`、`.editorconfig`、`.gitattributes` 配置编辑器分析与格式化；行尾 LF。clangd 读取 `build/compile_commands.json`（preset `CMAKE_EXPORT_COMPILE_COMMANDS=ON`）。项目用 MSVC 构建，clangd 自动翻译为 clang-cl 前端。

## 验证一次改动

构建+部署后启动 Palworld（装好 UE4SS experimental）。UE4SS Console 中看到 `MyPalMod loaded (v1.3.1)`。打开 UE4SS GUI → MyPalMod 标签 → 浮动窗口。各功能的结果打印到 Console 标签。

## 权威参考资料

- RE-UE4SS：https://github.com/UE4SS-RE/RE-UE4SS
- 创建 C++ mod：https://docs.ue4ss.com/guides/creating-a-c++-mod.html
- UHTHeaderDump（本地，`UHTHeaderDump/`）：Palworld 的类/字段/函数定义（gitignored）
- Palworld 运行时：https://steamcommunity.com/workshop/filedetails/?id=3625223587
