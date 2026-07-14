# MyPalMod — 面向 Palworld 1.0 的 UE4SS C++ mod

一套最小、可用的脚手架，用于以 **C++23**、**CMake** 和 **Ninja** 构建
[UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) C++ mod。

mod DLL 在构建期与具体游戏无关；Palworld 相关的行为由你装到游戏里的 UE4SS 运行时（Palworld 1.0 需要用
**experimental / 实验版**）提供。架构细节见 [CLAUDE.md](./CLAUDE.md)。

## 前置依赖

- **Visual Studio 2022**（最新版）—— *"使用 C++ 的桌面开发"*（Desktop development with C++）工作负载（提供 MSVC + Ninja）。
- **CMake ≥ 3.22**、**Git**。
- 已在 Palworld 中安装 **UE4SS experimental**（Steam 创意工坊："UE4SS Experimental" + PalSchema，或
  [GitHub release](https://github.com/UE4SS-RE/RE-UE4SS/releases)）。
- **不需要** Rust。

## 快速开始

所有命令都请在 **"x64 Native Tools Command Prompt for VS 2022"**（或 VS Developer PowerShell）中运行，
以保证 `cl.exe`/`ninja` 在 PATH 中。

```powershell
# 1. 克隆 RE-UE4SS 并初始化子模块
pwsh scripts/setup.ps1

# 2. 把部署目标指向你的游戏安装目录（包含 Pal/Binaries/Win64 的那个文件夹）
$env:PALWORLD_INSTALL_DIR = "C:\Program Files (x86)\Steam\steamapps\common\Palworld"

# 3. 配置 + 构建
cmake --preset ninja-msvc-x64
cmake --build --preset ninja-msvc-x64
#    -> build/Game__Shipping__Win64/bin/MyPalMod.dll

# 4. 部署到游戏并启用
cmake --build --preset ninja-msvc-x64 --target deploy
#    -> Pal/Binaries/Win64/ue4ss/Mods/MyPalMod/dlls/main.dll（+ enabled.txt）
```

启动 Palworld。UE4SS 控制台应打印 `MyPalMod loaded`，扫描结束后打印
`Object Name: /Script/CoreUObject.Object`。

## 目录结构

```
Palworld/
├── CMakeLists.txt        super-build：add_subdirectory(RE-UE4SS)；add_subdirectory(mods)
├── CMakePresets.json     Ninja + MSVC x64 preset（UE4SS_VERSION_CHECK=OFF）
├── cmake/Deploy.cmake    `deploy` 自定义 target -> 游戏 ue4ss/Mods/<ModName>/dlls/main.dll
├── scripts/              setup.ps1（克隆 RE-UE4SS）、build.ps1、deploy.ps1
├── RE-UE4SS/             由 setup.ps1 克隆（已 gitignore）——提供 UE4SS target
└── mods/MyPalMod/        mod 本体：CMakeLists.txt + src/dllmain.cpp
```

## 新增一个 mod

把 `mods/MyPalMod/` 复制为 `mods/<新名字>/`，修改其 `CMakeLists.txt` 里的 `TARGET` 和 `dllmain.cpp` 里的
类名，然后在父级 `add_subdirectory(<新名字>)`。如果你复用 `cmake/Deploy.cmake`，每个 mod 都会有自己的
`deploy` 形式 target。

## 注意事项

- Ninja 是单配置生成器；preset **显式设置** `CMAKE_BUILD_TYPE=Game__Shipping__Win64`（UE4SS 的默认 triplet）。
  必须显式设置——否则 imgui 依赖（经其 examples 的 `if(NOT CMAKE_BUILD_TYPE) set(Debug FORCE)`）会把默认改成
  `Debug`，导致 UE4SS 的 triplet 宏不生效、编译失败。
- 如果你想用加载顺序控制来启用 mod（而不是用 `enabled.txt`），可在
  `Pal/Binaries/Win64/ue4ss/Mods/mods.txt` 中 `Keybinds` 行的**上方**加一行 `MyPalMod : 1`。

## 编辑器（clangd / clang-format / clang-tidy）

`.clangd`、`.clang-format` 和 `.clang-tidy` 启用了基于 clangd 的静态分析。clangd 读取
`build/compile_commands.json`（结构变动后请重新运行 `cmake --preset ninja-msvc-x64` 来重新生成）。本工程用
MSVC 构建，所以 clangd 会自动把 `cl.exe` 的选项翻译成它的 clang-cl 前端。clang-tidy 的作用范围限定在
`mods/`，从不分析第三方 `RE-UE4SS/` 头文件。如果 clangd 找不到 Windows SDK，请给它 MSVC 驱动，例如
VS Code：`"clangd.arguments": ["--query-driver=C:/Program Files/Microsoft Visual Studio/**/Hostx64/x64/cl.exe"]`。
行尾通过 `.gitattributes`/`.editorconfig` 统一为 LF。

## 参考资料

- [UE4SS C++ mod 模板](https://github.com/UE4SS-RE/UE4SSCPPTemplate)
- [创建 C++ mod](https://docs.ue4ss.com/guides/creating-a-c++-mod.html)
- [安装 C++ mod](https://docs.ue4ss.com/dev/guides/installing-a-c++-mod.html)
- [RE-UE4SS](https://github.com/UE4SS-RE/RE-UE4SS)
