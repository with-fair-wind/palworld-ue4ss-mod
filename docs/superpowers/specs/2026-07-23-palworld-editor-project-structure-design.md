# PalworldEditor 项目结构调整设计

## 目标

将当前占位名称 `MyPalMod` 统一替换为能够表达物品、背包和帕鲁技能编辑能力的
`PalworldEditor`，并把头文件从 `src/` 迁移到按职责划分的 `inc/` 子目录。根工程继续作为
UE4SS super-build，可显式注册多个相互独立的 Mod。

## 目录结构

```text
mods/
├─ CMakeLists.txt
└─ PalworldEditor/
   ├─ CMakeLists.txt
   ├─ inc/
   │  ├─ game/
   │  │  └─ pal_game.hpp
   │  ├─ skills/
   │  │  ├─ pal_skills.hpp
   │  │  ├─ skill_catalog.hpp
   │  │  └─ skill_editor_service.hpp
   │  └─ support/
   │     └─ text_encoding.hpp
   ├─ src/
   │  ├─ dllmain.cpp
   │  └─ pal_skills.cpp
   └─ tests/
      └─ skill_editor_tests.cpp
```

每个 Mod 的 `inc/` 只作为该 Mod target 的私有包含根目录。包含语句使用
`<game/pal_game.hpp>`、`<skills/pal_skills.hpp>`、`<support/text_encoding.hpp>` 等模块路径。
`src/` 保持扁平，不再划分子目录。

根 `mods/CMakeLists.txt` 继续显式调用 `add_subdirectory()`。新增其他 Mod 时，每个 Mod 都拥有自己的
`CMakeLists.txt`、`inc/`、`src/` 和可选 `tests/`，避免自动目录扫描把非 Mod 文件夹加入构建。

## 命名

- Mod 目录、CMake target、DLL 输出名、部署目录和 UE4SS `ModName`：`PalworldEditor`
- C++ 实现类：`PalworldEditorMod`
- 测试 target：`PalworldEditorTests`
- CTest 名称：`PalworldEditor.SkillEditor`
- GUI 标签、窗口标题、日志前缀和 Hook owner：`PalworldEditor`

不保留 `MyPalMod` target 或运行时名称的兼容别名，避免新旧名称并存。历史设计与实施文档保留原始名称，
作为当时决策的记录。

## 物品目录

删除 `item_database.h` 及其不完整的静态物品 ID 回退列表。物品目录只由
`pal_game::scan_all_items()` 从当前 UE4SS 运行时已加载的 `PalStaticItemData*` UObject 的 `ID` 属性生成，
无需解包游戏资源。

`on_unreal_init()` 成功进入可使用 Unreal API 的生命周期后，设置一次物品扫描请求；实际扫描仍在
`on_update()` 的游戏线程中执行。用户可以通过现有按钮重新扫描。若运行时尚未加载任何匹配对象或扫描失败，
列表保持为空并显示“尚未发现物品，请重新扫描”，不再展示静态回退数据。

该扫描能够完整覆盖当时已经加载到 UObject 系统中的匹配物品定义，但不声称覆盖尚未加载的资产。

## CMake 与部署

`PalworldEditor` target 的源文件仍由 CMake 显式列出。其私有包含目录改为
`${CMAKE_CURRENT_SOURCE_DIR}/inc`；测试 target 使用相同包含根。部署 helper 接收新 target 名称，因此生成
`Pal/Binaries/Win64/ue4ss/Mods/PalworldEditor/dlls/main.dll` 和对应的 `enabled.txt`。

clang-format 与 clang-tidy target 继续从 Git 跟踪的 `mods/` 文件和 `compile_commands.json` 中确定范围，
目录迁移后无需增加默认构建依赖。

## 文档与兼容性

README 中的标题、构建命令、产物路径、部署路径、使用说明和目录树全部切换到新名称与新结构。
`cmake/Deploy.cmake` 中仅作为说明的旧 target 示例同步更新。用户在游戏安装目录中已有的
`MyPalMod` 部署不会被自动删除；部署新版本后应由用户停用或移除旧目录，避免两个 DLL 同时加载。

## 验证

完成迁移后执行以下验证：

1. 重新运行 `cmake --preset ninja-msvc-x64`，刷新 `compile_commands.json`。
2. 运行 `format-check`，确认移动后的 C++ 文件符合项目格式。
3. 构建 `PalworldEditor` 和 `PalworldEditorTests`。
4. 运行 CTest，确认 `PalworldEditor.SkillEditor` 通过。
5. 运行 `tidy-check`，确认新目录能够被静态分析 target 正确发现。
6. 查询 Ninja target，确认 `PalworldEditor` 存在且 `MyPalMod` 不再存在。
7. 检查 Git diff，确认没有修改 `RE-UE4SS/` 或用户已有的无关文件。
