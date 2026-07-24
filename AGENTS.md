# AGENTS.md

This file provides guidance to Codex (Codex.ai/code) when working with code in this repository.

> 本文件为 Codex 在本仓库中工作时提供指引。

## 这是什么

一个面向 **Palworld 1.0** 的 **UE4SS C++ mod** 工程（C++23 / CMake / Ninja）。当前 mod 名为
`PalworldEditor`（版本 1.4.2），构建产物是 `PalworldEditor.dll`。

该 mod 通过 UE4SS GUI 提供物品浏览与修改、背包数量修改，以及 Q/E 当前选中的下一只待出战帕鲁的
主动/被动技能编辑。
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

`PalworldEditorTests`/CTest 覆盖不依赖 Unreal 的物品目录、技能目录与技能编辑服务逻辑。反射调用、ImGui 和
Palworld 存档效果仍需游戏内端到端验证。

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
- `inc/items/item_catalog.hpp`：本地化物品标签、搜索、去重和索引；
- `inc/skills/skill_catalog.hpp`：可搜索的主动/被动技能目录；
- `inc/skills/skill_editor_service.hpp`：编辑校验、FIFO 请求、重读和回滚；
- `inc/skills/selected_target_state.hpp`：当前目标切换检测和过期编辑请求保护；
- `inc/skills/pal_skills.hpp` + `src/pal_skills.cpp`：领域服务到 Palworld UFunction 的适配；
- `src/dllmain.cpp`：mod 生命周期、ImGui 和线程间请求交接。

**Mod 入口点契约**（`mods/PalworldEditor/src/dllmain.cpp`）：`PalworldEditorMod` 继承
`RC::CppUserModBase`，设置元数据并重写 `on_update`、`on_unreal_init`；DLL 导出 `start_mod()`（构造实例）
和 `uninstall_mod()`（销毁实例）。日志用
`RC::Output::send<LogLevel::Verbose>(STR("...{ }...\n"))`（底层是 std::format；`STR()` 会选择正确的字符
宽度）。Unreal API（`RC::Unreal::*`、`UObjectGlobals` 等）只能在 `on_unreal_init()` 内部及之后使用。

ImGui 回调与游戏线程之间只传递标准库快照、互斥锁保护的请求参数和原子请求标志。所有 UObject 指针都视为
非拥有句柄；业务数据的反射读取和修改只在 `on_update()` 所在游戏线程执行。当前技能目标从本地
`PlayerController` 世界上下文解析，用户点击“选择当前帕鲁”后以 `FPalInstanceID.InstanceId` 和目标代数确认；
Q/E 切换会取消选择并清空旧请求。不缓存扫描得到的帕鲁对象，也不注册详情页函数 Hook。

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
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor PalworldEditorTests
ctest --test-dir build --output-on-failure
git diff --check
```

构建并部署后启动 Palworld 1.0。UE4SS 控制台应出现 `PalworldEditor loaded (v1.4.2)`；打开 UE4SS GUI 的
`PalworldEditor` 页签后应能看到浮动窗口。至少验证物品扫描与本地化标签、背包读取、Q/E 选定帕鲁后点击
“选择当前帕鲁”、Q/E 切换时编辑区失效、被动技能新增/替换/删除，以及主动技能装备/替换/清空。若 mod 未加载，
检查安装路径、`dlls/main.dll` 命名，以及 `enabled.txt`/`mods.txt`。

## 权威参考资料

- 官方模板：https://github.com/UE4SS-RE/UE4SSCPPTemplate
- 创建 C++ mod：https://docs.ue4ss.com/guides/creating-a-c++-mod.html
- 安装 C++ mod：https://docs.ue4ss.com/dev/guides/installing-a-c++-mod.html
- RE-UE4SS（框架 + 构建系统）：https://github.com/UE4SS-RE/RE-UE4SS
- Palworld 1.0 运行时：https://github.com/UE4SS-RE/RE-UE4SS/releases ,
  https://steamcommunity.com/workshop/filedetails/?id=3625223587
