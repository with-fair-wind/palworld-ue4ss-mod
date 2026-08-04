# 仓库代码重构 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 整理 `mods/PalworldEditor/` 代码组织——头文件统一在 `inc/`、共用反射原语提取到 `common/`、`src/` 按功能分文件夹、`dllmain.cpp` 拆分 render 实现。不改变运行时行为。

**Architecture:** 三阶段机械重构（文件归位 → 提取 common → 提取 mod_core + 拆 dllmain），每阶段以"构建 + ctest 全通过"为唯一验收标准。无新功能、无逻辑变更、无多 CMake。

**Tech Stack:** C++23、UE4SS 反射、CMake/Ninja、现有 `PalworldEditorTests` / `PalworldEditorBaseResourceSharingTests`。

## Global Constraints

- **不改变任何运行时行为**：不修改反射路径、UFunction 调用、属性偏移、Hook 逻辑。纯文件移动 + include 路径更新 + 类声明提取。
- **不升版本号**：纯重构。
- **每阶段构建通过才进下一阶段**：`format-check + PalworldEditor + PalworldEditorTests + PalworldEditorBaseResourceSharingTests + ctest` 全绿。
- **使用 `git mv`** 移动文件（保留 git 历史）。
- **include 路径策略**：移动后用 `grep -rn "旧路径" mods/` 找到所有引用，批量更新；然后构建，修剩余编译错误。
- 所有命令在 **VS x64 开发者环境**中运行（`vcvarsall.bat x64`）。

---

## Phase 1：文件归位（src/ 按功能分文件夹 + 移走 stray header）

**目标**：`src/` 下 8 个 `.cpp` 按功能分子目录；`src/pal_base_resource_runtime.hpp` 移到 `inc/base_resource_sharing/`。

### Task 1.1：移走 stray header + 建 src 子目录

**Files:**
- Move: `src/pal_base_resource_runtime.hpp` → `inc/base_resource_sharing/pal_base_resource_runtime.hpp`
- Move: `src/dllmain.cpp` → `src/mod/dllmain.cpp`
- Move: `src/pal_skills.cpp` → `src/skills/pal_skills.cpp`
- Move: `src/pal_stats.cpp` → `src/pal_stats/pal_stats.cpp`
- Move: `src/pal_identity.cpp` → `src/pal_identity/pal_identity.cpp`
- Move: `src/grapple_cooldown_gateway.cpp` → `src/grappling_hook/grapple_cooldown_gateway.cpp`
- Move: `src/pal_base_resource_runtime.cpp` → `src/base_resource_sharing/pal_base_resource_runtime.cpp`
- Move: `src/pal_base_resources.cpp` → `src/base_resource_sharing/pal_base_resources.cpp`

- [ ] **Step 1：用 git mv 移动所有文件**

```bash
mkdir -p src/mod src/items src/skills src/grappling_hook src/base_resource_sharing src/pal_identity src/pal_stats

git mv src/pal_base_resource_runtime.hpp inc/base_resource_sharing/pal_base_resource_runtime.hpp
git mv src/dllmain.cpp src/mod/dllmain.cpp
git mv src/pal_skills.cpp src/skills/pal_skills.cpp
git mv src/pal_stats.cpp src/pal_stats/pal_stats.cpp
git mv src/pal_identity.cpp src/pal_identity/pal_identity.cpp
git mv src/grapple_cooldown_gateway.cpp src/grappling_hook/grapple_cooldown_gateway.cpp
git mv src/pal_base_resource_runtime.cpp src/base_resource_sharing/pal_base_resource_runtime.cpp
git mv src/pal_base_resources.cpp src/base_resource_sharing/pal_base_resources.cpp
```

- [ ] **Step 2：更新 CMakeLists.txt 源列表**

把 `add_library(${TARGET} SHARED ...)` 中的源文件改为新路径：

```cmake
add_library(${TARGET} SHARED
    src/mod/dllmain.cpp
    src/skills/pal_skills.cpp
    src/pal_stats/pal_stats.cpp
    src/pal_identity/pal_identity.cpp
    src/grappling_hook/grapple_cooldown_gateway.cpp
    src/base_resource_sharing/pal_base_resource_runtime.cpp
    src/base_resource_sharing/pal_base_resources.cpp
)
```

- [ ] **Step 3：更新 pal_base_resource_runtime.hpp 的 include 路径**

移动后，引用它的文件需要更新 include 路径。查找所有引用：

```bash
grep -rn "pal_base_resource_runtime.hpp" mods/PalworldEditor/
```

预期引用文件（改为新路径）：
- `src/base_resource_sharing/pal_base_resource_runtime.cpp`：`#include "pal_base_resource_runtime.hpp"` 不变（同目录，但 C++ include 以 inc/ 为根，需改为 `<base_resource_sharing/pal_base_resource_runtime.hpp>`）。
- `inc/base_resource_sharing/pal_base_resources.hpp`（如果有引用）。
- `src/mod/dllmain.cpp`（如果有引用）。

每处把 `#include "pal_base_resource_runtime.hpp"` 或 `#include "../src/pal_base_resource_runtime.hpp"` 改为 `#include <base_resource_sharing/pal_base_resource_runtime.hpp>`。

- [ ] **Step 4：构建 + 验证**

```powershell
cmake --preset ninja-msvc-x64
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor PalworldEditorTests PalworldEditorBaseResourceSharingTests
ctest --test-dir build --output-on-failure
```

Expected：全部构建 + 测试通过。如果有编译错误（include 路径遗漏），用 grep 找到 + 修正 + 重建。

- [ ] **Step 5：提交**

```bash
git add -A
git commit -m "refactor: organize src/ by feature + move stray header to inc/"
```

---

## Phase 2：提取 `common/`（共用反射原语）

**目标**：把 `pal_game.hpp` 中的跨功能反射原语提取到 `inc/common/game_reflection.hpp`；`text_encoding.hpp` 移到 `common/`。

### Task 2.1：创建 `inc/common/game_reflection.hpp` + 移 text_encoding

**Files:**
- Create: `inc/common/game_reflection.hpp`
- Move: `inc/support/text_encoding.hpp` → `inc/common/text_encoding.hpp`

- [ ] **Step 1：创建 `inc/common/game_reflection.hpp`**

从 `inc/game/pal_game.hpp` 中**提取以下函数**到新文件（保留原文件的 `namespace pal_game {}` 命名空间）：

需要提取的函数（仅这些——它们是被多个功能模块共用的反射原语）：
- `is_valid(UObject*)`
- `invoke_object_return(UObject*, const TCHAR*)`
- `invoke_bool_return(UObject*, const TCHAR*)`
- `kInventoryClassName`

新文件 `inc/common/game_reflection.hpp` 结构：
```cpp
#pragma once
// ... UE4SS includes (UObject, UObjectGlobals, etc.) ...
namespace pal_game {
// is_valid, invoke_object_return, invoke_bool_return, kInventoryClassName
}
```

原文件 `inc/game/pal_game.hpp` 保留**功能特化**函数（`give_items`、`set_slot_count`、`scan_all_items`、`read_inventory`、`resolve_selected_otomo`、`resolve_local_otomo_holder`、`discover_objects`、`get_main_container` 等），并在顶部 `#include <common/game_reflection.hpp>`（因为它们依赖 `is_valid` 等）。

- [ ] **Step 2：移 text_encoding**

```bash
git mv inc/support/text_encoding.hpp inc/common/text_encoding.hpp
```

- [ ] **Step 3：批量更新 include 路径**

```bash
# 查找所有引用旧路径的文件
grep -rn "support/text_encoding" mods/PalworldEditor/
grep -rn "game/pal_game.hpp" mods/PalworldEditor/
```

替换规则：
- `#include <support/text_encoding.hpp>` → `#include <common/text_encoding.hpp>`
- `#include <game/pal_game.hpp>` → 保持不变（pal_game.hpp 仍在 game/，只是 include 了 common/game_reflection.hpp）
- 需要直接用 `is_valid`/`invoke_*` 的文件：增加 `#include <common/game_reflection.hpp>`

- [ ] **Step 4：构建 + 验证**

```powershell
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor PalworldEditorTests PalworldEditorBaseResourceSharingTests
ctest --test-dir build --output-on-failure
```

Expected：全部通过。如有 include 遗漏，grep + 修 + 重建。

- [ ] **Step 5：提交**

```bash
git add -A
git commit -m "refactor: extract common reflection primitives to common/"
```

---

## Phase 3：提取 `mod_core.hpp` + 拆 dllmain

**目标**：把 `PalworldEditorMod` 类声明从头文件化（`inc/mod/mod_core.hpp`），把 render 函数实现分到各功能的 `*_ui.cpp`。

### Task 3.1：提取 `PalworldEditorMod` 类声明到 `inc/mod/mod_core.hpp`

**Files:**
- Create: `inc/mod/mod_core.hpp`
- Modify: `src/mod/dllmain.cpp`（删除类声明，改为 include 头文件）

- [ ] **Step 1：创建 `inc/mod/mod_core.hpp`**

把 `src/mod/dllmain.cpp` 中 `PalworldEditorMod` 类的**完整声明**（class 定义 + 所有成员变量、嵌套类型如 `SkillEditorSnapshot`、static render 函数声明、回调函数声明）提取到 `inc/mod/mod_core.hpp`。

注意事项：
- 类中使用的 UE4SS 类型（CppUserModBase、Hook::GlobalCallbackId、UObject 等）需要 include 对应头文件。
- 类中使用的功能模块类型（skill_editor::、pal_stats::、item_catalog:: 等）需要 include 对应功能头文件。
- 所有 include 放在 `mod_core.hpp` 顶部。

`src/mod/dllmain.cpp` 变为：
```cpp
#include <mod/mod_core.hpp>
// ... 其他实现专用 include ...
// PalworldEditorMod 的成员函数实现（构造/析构/回调/game_thread_tick/transition）
// static render 函数的实现（暂时全留在这里，Task 3.2 再拆）
```

- [ ] **Step 2：构建 + 验证**

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditor
```

Expected：编译通过（所有 .cpp 现在 include `mod_core.hpp` 获取类声明）。如缺少 include，补上。

- [ ] **Step 3：提交**

```bash
git add -A
git commit -m "refactor: extract PalworldEditorMod class declaration to mod_core.hpp"
```

### Task 3.2：拆分 render 函数到各功能 ui.cpp

**Files:**
- Create: `src/items/item_ui.cpp`（render_give_items + render_item_browser + render_inventory）
- Create: `src/skills/skill_ui.cpp`（render_passive_skills + render_active_skills + render_pal_editor）
- Create: `src/base_resource_sharing/base_resource_ui.cpp`（render_base_resource_sharing + render_grapple_no_cooldown）
- Create: `src/pal_stats/stat_ui.cpp`（render_pal_stats）
- Modify: `src/mod/dllmain.cpp`（删除已移走的 render 函数实现）
- Modify: `CMakeLists.txt`（添加新 .cpp 到源列表）

- [ ] **Step 1：创建各 ui.cpp 并移入 render 实现**

对每个功能模块：
1. 创建 `src/<feature>/<feature>_ui.cpp`。
2. 文件顶部 `#include <mod/mod_core.hpp>`（获取类声明 + static 函数声明）+ 该功能需要的其他 include。
3. 从 `src/mod/dllmain.cpp` 中**剪切**对应 `render_*` 函数的实现，**粘贴**到新文件。
4. `dllmain.cpp` 中只保留声明（在 `mod_core.hpp` 里），实现已移走。

每个 ui.cpp 的骨架：
```cpp
#include <mod/mod_core.hpp>
// + 该功能的依赖 include（imgui、功能头等）

// render_xxx 函数实现（从 dllmain.cpp 移来，签名不变——它们是 static 成员）
```

- [ ] **Step 2：更新 CMakeLists.txt**

```cmake
add_library(${TARGET} SHARED
    src/mod/dllmain.cpp
    src/items/item_ui.cpp
    src/skills/pal_skills.cpp
    src/skills/skill_ui.cpp
    src/grappling_hook/grapple_cooldown_gateway.cpp
    src/base_resource_sharing/pal_base_resource_runtime.cpp
    src/base_resource_sharing/pal_base_resources.cpp
    src/base_resource_sharing/base_resource_ui.cpp
    src/pal_identity/pal_identity.cpp
    src/pal_stats/pal_stats.cpp
    src/pal_stats/stat_ui.cpp
)
```

- [ ] **Step 3：构建 + 格式检查 + 验证**

```powershell
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor PalworldEditorTests PalworldEditorBaseResourceSharingTests
ctest --test-dir build --output-on-failure
git diff --check
```

Expected：全部通过。dllmain.cpp 显著变瘦；各 ui.cpp 各自负责本功能的 render。

- [ ] **Step 4：提交**

```bash
git add -A
git commit -m "refactor: split dllmain render implementations into per-feature ui modules"
```

---

## 最终验证

- [ ] **构建 + 测试 + 格式 + git diff --check**

```powershell
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor PalworldEditorTests PalworldEditorBaseResourceSharingTests
ctest --test-dir build --output-on-failure
git diff --check
git status --short
```

Expected：三目标构建成功；CTest 全通过；`git diff --check` 无输出；工作树干净。

- [ ] **确认目录结构**

```bash
find mods/PalworldEditor/inc mods/PalworldEditor/src -name "*.hpp" -o -name "*.cpp" | sort
```

Expected：所有 `.hpp` 在 `inc/` 下；`src/` 下只有 `.cpp`（按功能分子目录）；`src/` 下无 `.hpp`。

- [ ] **游戏内冒烟测试（可选但推荐）**

部署 + 进游戏，确认 mod 正常加载、各功能界面正常（因为不改逻辑，不应有功能回归）。
