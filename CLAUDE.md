# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

> 本文件为 Claude Code 在本仓库中工作时提供指引。

## 这是什么

一个面向 **Palworld 1.0** 的最小 **UE4SS C++ mod** 工程（C++23 / CMake / Ninja）。构建产物是一个单一
DLL（`MyPalMod.dll`），由 UE4SS 在运行时加载。

重要的架构事实：**mod DLL 在构建期与具体游戏无关。** 所有游戏相关的行为（对象内存布局、函数偏移、版本
兼容性）都由安装在游戏中的 **UE4SS 运行时**加上自定义游戏配置负责——而不是由本仓库里的任何东西负责。
Palworld 1.0 特别需要 UE4SS 的 **experimental（实验版）** 运行时（Steam 创意工坊里的 "UE4SS Experimental"
+ PalSchema，或 GitHub release）。本仓库里没有任何地方写死 "Palworld"；这套脚手架只是基于 RE-UE4SS 的
`UE4SS` target 进行构建，其余由对应的运行时完成。

## 前置依赖

- **Visual Studio 2022**（最新版），勾选 *"使用 C++ 的桌面开发"*（Desktop development with C++）工作负载——
  提供 MSVC（`cl.exe`）和 Ninja。C++23 通过 `/std:c++latest` 启用，因此需要较新的 VS 2022。
- **CMake ≥ 3.22**，在 PATH 中。
- **Git**，在 PATH 中（供 `scripts/setup.ps1` 使用）。
- **不需要 Rust。** RE-UE4SS 的版本检查（默认开启）会要求 MSVC ≥ 1943 *且* Rust ≥ 1.73；preset 里设置了
  `UE4SS_VERSION_CHECK=OFF`，而且我们只会构建自己的 mod target（它依赖 `UE4SS` 静态库，而不依赖那个需要
  Rust 的独立 `UVTD` 程序）。

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

# 4. 构建   -> build/Game__Shipping__Win64/bin/MyPalMod.dll
cmake --build --preset ninja-msvc-x64

# 5. 部署到游戏 -> Pal/Binaries/Win64/Mods/MyPalMod/dlls/main.dll（+ enabled.txt）
cmake --build --preset ninja-msvc-x64 --target deploy

# 完全重新构建
Remove-Item -Recurse -Force build ; cmake --preset ninja-msvc-x64 ; cmake --build --preset ninja-msvc-x64
```

本工程没有测试套件。验证方式是游戏内的端到端验证（见下文「验证一次改动」）。

## 架构

**Super-build 布局。** 根目录的 `CMakeLists.txt` 只做两件事：`add_subdirectory(RE-UE4SS)`（由
`scripts/setup.ps1` 克隆、已被 gitignore）和 `add_subdirectory(mods)`。RE-UE4SS 定义了 `UE4SS`
静态库 target，它会传递性地提供 mod 所需的全部头文件、编译选项和宏定义——包括 C++23 语言标准。
`mods/` 下每个 mod 都是一个链接 `UE4SS` 的 `SHARED` 库。要修改 mod target，只需编辑
`mods/MyPalMod/CMakeLists.txt`；要新增一个 mod，复制该文件夹后再 `add_subdirectory()` 即可。

**UE4SS 的三元组（triplet）构建系统 + Ninja。** RE-UE4SS 定义了它自己的构建 "triplet"
（`Game__Shipping__Win64`、`CasePreserving__Dev__Win64`……），由它们驱动编译宏定义（`UE_GAME`、
`UE_BUILD_SHIPPING`、`PLATFORM_WINDOWS` 等）的组合——只有当 `$<CONFIG>` 等于某个 triplet 时这些宏才会生效。
Ninja 是单配置（single-config）生成器，所以 preset **显式设置** `CMAKE_BUILD_TYPE=Game__Shipping__Win64`
（即 UE4SS 的默认值）。**必须显式设置**：RE-UE4SS 拉取的 imgui 依赖里，其 examples 含有
`if(NOT CMAKE_BUILD_TYPE) set(CMAKE_BUILD_TYPE Debug ... FORCE)`；若不显式指定，这个默认就会"赢"，使
`$<CONFIG>` 变成 `Debug` 而不匹配任何 triplet，UE4SS 的关键宏不会被定义，编译会失败。这就是输出 DLL 落在
`build/Game__Shipping__Win64/bin/` 的原因。

**Mod 入口点契约**（`mods/MyPalMod/src/dllmain.cpp`）：一个继承自 `RC::CppUserModBase` 的类设置元数据
（`ModName`、`ModVersion`、`ModDescription`、`ModAuthors`）并重写生命周期钩子（`on_update`、
`on_unreal_init`）；DLL 导出 `start_mod()`（构造实例）和 `uninstall_mod()`（销毁实例）。日志用
`RC::Output::send<LogLevel::Verbose>(STR("...{ }...\n"))`（底层是 std::format；`STR()` 会选择正确的字符
宽度）。Unreal API（`RC::Unreal::*`、`UObjectGlobals` 等）只能在 `on_unreal_init()` 内部及之后使用。

**部署契约。** C++ mod 安装到游戏 `Pal/Binaries/Win64/` 下的 `Mods/<ModName>/dlls/main.dll`（把构建出的
DLL 改名；用 `<ModName>.dll` 也可以）。启用方式：在 mod 文件夹里放一个空的 `enabled.txt`，**或**者在
`Mods/mods.txt` 中 `Keybinds` 行的上方加一行 `<ModName> : 1`。`deploy` target（`cmake/Deploy.cmake`）通过
`$<TARGET_FILE:MyPalMod>` 自动完成这件事，因此无论当前 triplet 输出目录是哪个，源文件路径都始终正确。

## 工具链（clangd / clang-tidy / clang-format）

`.clangd`、`.clang-tidy`、`.clang-format`、`.editorconfig` 和 `.gitattributes` 负责编辑器内的分析与格式化；
行尾统一为 LF。

clangd 读取 `build/compile_commands.json`（preset 里设置了 `CMAKE_EXPORT_COMPILE_COMMANDS=ON`）。本工程用
MSVC 构建，所以该文件里记录的是 `cl.exe` 调用，clangd 会自动把它们翻译成自己的 clang-cl 前端——不需要单独的
clang 工具链。修改 `CMakeLists.txt` 或新增源文件后，请重新运行 `cmake --preset ninja-msvc-x64` 来刷新它。

- clang-tidy 通过 `.clang-tidy` 在 clangd 内部运行；它只分析 `mods/` 下的文件，从不分析第三方 `RE-UE4SS/`
  头文件（`HeaderFilterRegex: 'mods[/\]'`）。`.clangd` 还会在 `RE-UE4SS/` 内部屏蔽 clangd 自身的诊断。
- `.clang-format` = Allman 大括号、4 空格缩进、120 列上限（与现有代码一致）。
- 如果 clangd 报 "system header not found" / 找不到 Windows SDK，请允许它查询 MSVC 驱动——例如 VS Code：
  `"clangd.arguments": ["--query-driver=C:/Program Files/Microsoft Visual Studio/**/Hostx64/x64/cl.exe"]`。

## 验证一次改动

构建并部署后，在装好 UE4SS 实验版运行时的情况下启动 Palworld 1.0。UE4SS 控制台里应当能看到
`MyPalMod loaded`（蓝色，靠近顶部），扫描结束后看到 `Object Name: /Script/CoreUObject.Object`。同样的内容
也会写入 UE4SS 日志文件。如果两者都没出现，说明 DLL 没被加载——检查安装路径、`dlls/main.dll` 命名，以及
`enabled.txt`/`mods.txt` 里的条目。

## 权威参考资料

- 官方模板：https://github.com/UE4SS-RE/UE4SSCPPTemplate
- 创建 C++ mod：https://docs.ue4ss.com/guides/creating-a-c++-mod.html
- 安装 C++ mod：https://docs.ue4ss.com/dev/guides/installing-a-c++-mod.html
- RE-UE4SS（框架 + 构建系统）：https://github.com/UE4SS-RE/RE-UE4SS
- Palworld 1.0 运行时：https://github.com/UE4SS-RE/RE-UE4SS/releases ,
  https://steamcommunity.com/workshop/filedetails/?id=3625223587
