# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

> 本文件为 Claude Code 在本仓库中工作时提供指引。详细的用户文档以 `README.md` 为准。

## 项目概览

这是一个面向 **Palworld 1.0** 的 UE4SS C++23 mod。当前 mod 名为 `PalworldEditor`（版本 1.4.1），通过
UE4SS GUI 提供：

- 运行时物品目录和当前语言名称搜索；
- 给予物品、读取主背包和修改槽位数量；
- 每帧识别 Q/E 当前选中的下一只待出战帕鲁；
- 被动技能新增、替换、删除；
- 三个 `EquipWaza` 主动技能槽位的装备、替换和清空。

mod 本体通过 `/Script/Pal.*` 路径和 Palworld 类型进行反射调用，因此是 Palworld 专用实现。Palworld 1.0
需要 UE4SS Experimental (Palworld) + PalSchema（含 `MemberVariableLayout.ini`）。F10 游戏控制台不可用，
所有用户交互通过 UE4SS GUI 的 `PalworldEditor` 页签完成。

## 前置依赖与命令

- Visual Studio 2022 最新版，安装“使用 C++ 的桌面开发”工作负载；
- CMake ≥ 3.22、Git；
- Rust stable（`cargo` / `rustc`）；RE-UE4SS 的 `UE4SS` target 会构建 Rust 实现的 PatternSleuth；
- 所有 CMake 构建命令必须在 VS x64 开发者环境中运行。

```powershell
pwsh scripts/setup.ps1
$env:PALWORLD_INSTALL_DIR = "F:\...\Palworld"  # 可选；必须在配置前设置
cmake --preset ninja-msvc-x64
cmake --build --preset ninja-msvc-x64 --target PalworldEditor

cmake --build --preset ninja-msvc-x64 --target PalworldEditorTests
ctest --test-dir build --output-on-failure

cmake --build --preset ninja-msvc-x64 --target deploy
```

DLL 输出为 `build/Game__Shipping__Win64/bin/PalworldEditor.dll`；部署目标为
`Pal/Binaries/Win64/ue4ss/Mods/PalworldEditor/dlls/main.dll`。

## 架构

```text
mods/PalworldEditor/
├── inc/
│   ├── game/pal_game.hpp
│   ├── items/item_catalog.hpp
│   ├── skills/pal_skills.hpp
│   ├── skills/selected_target_state.hpp
│   ├── skills/skill_catalog.hpp
│   ├── skills/skill_editor_service.hpp
│   └── support/text_encoding.hpp
├── src/
│   ├── dllmain.cpp
│   └── pal_skills.cpp
└── tests/
    └── skill_editor_tests.cpp
```

- `pal_game.hpp`：背包、物品定义、当前待出战帕鲁和诊断扫描的反射访问；
- `item_catalog.hpp`：物品标签、搜索、去重、排序和 Raw ID 索引；
- `skill_catalog.hpp`：主动/被动技能目录的纯逻辑；
- `skill_editor_service.hpp`：编辑校验、FIFO 请求、操作后重读和失败回滚；
- `selected_target_state.hpp`：当前目标切换检测和过期编辑请求保护；
- `pal_skills.*`：领域接口到 Palworld UFunction 的适配；
- `dllmain.cpp`：`PalworldEditorMod` 生命周期、ImGui 和线程间请求交接。

ImGui 回调只读写标准库 UI 状态、原子请求标志以及互斥锁保护的快照/参数。所有 UObject 反射读取和修改都在
`on_update()` 所在游戏线程执行。当前目标每帧从 `PalPlayerInventoryData` 世界上下文解析，扫描结果中的
UObject 指针不会跨帧缓存，也不再依赖帕鲁详情页 Hook。

物品和技能界面显示 `本地化名称 [RawId]`，但游戏调用始终使用 Raw ID；背包修改使用槽位索引。主动技能通过
`ClearEquipWaza()` 后按顺序调用 `AddEquipWaza()` 重写完整装备列表，失败时由领域服务尝试恢复原状态。

## 工具链与验证

clangd 读取 `build/compile_commands.json`；`.clang-format`、`.clang-tidy`、`.clangd`、`.editorconfig` 和
`.gitattributes` 位于仓库根目录。常用手动 target：

```powershell
cmake --build --preset ninja-msvc-x64 --target format
cmake --build --preset ninja-msvc-x64 --target format-check
cmake --build --preset ninja-msvc-x64 --target tidy
cmake --build --preset ninja-msvc-x64 --target tidy-check
```

Windows 下 `tidy-check` 会用单进程解析所有 `mods/` 翻译单元及其 RE-UE4SS/Unreal 依赖，可能长时间没有输出。
提交前至少执行：

```powershell
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor PalworldEditorTests
ctest --test-dir build --output-on-failure
git diff --check
```

游戏内验证时，UE4SS 控制台应出现 `PalworldEditor loaded (v1.4.1)`；打开 `PalworldEditor` 页签后验证物品、
背包、Q/E 当前待出战帕鲁识别，以及主动/被动技能编辑。反射签名和 UFunction 参数布局来自 Palworld 1.0，游戏更新后可能
需要结合本地 `UHTHeaderDump/` 重新核对。
