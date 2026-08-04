# 仓库代码重构 Design

**日期：** 2026-08-01
**目标版本：** 在当前最新代码（1.6.10）之上，不改变功能逻辑、不升版本号。

## 目标

整理 `mods/PalworldEditor/` 的代码组织：头文件统一在 `inc/`、共用接口提取到 `common/`、`src/` 按功能分文件夹、`dllmain.cpp` 按功能拆分 render 实现。**不改变任何运行时行为**；纯结构调整。

## 非目标

- 不做多 CMake（单一 DLL target）。
- 不改变功能逻辑、反射路径、UFunction 调用。
- 不升版本号（纯重构）。
- 不新增功能。

## 现状

- `inc/` 已按功能分文件夹（items/skills/grappling_hook/base_resource_sharing/pal_identity/pal_stats/game/support）。
- **唯一在 `src/` 的头文件**：`src/pal_base_resource_runtime.hpp`。
- `src/` 8 个 `.cpp` 扁平放置，未按功能分文件夹。
- `dllmain.cpp` 是最大文件（~1600 行），混了 mod 生命周期 + 全部 GUI render + 游戏线程调度。
- `game/pal_game.hpp` 混了共用反射原语（is_valid / invoke_*）和功能特化函数（give_items / resolve_selected_otomo / scan_all_items）。

## 目标结构

```
mods/PalworldEditor/
├── inc/
│   ├── common/                          ← 新：跨功能共用基础设施
│   │   ├── text_encoding.hpp            （从 support/ 移入）
│   │   └── game_reflection.hpp          （从 pal_game.hpp 提取：is_valid / invoke_object_return
│   │                                      / invoke_bool_return / kInventoryClassName）
│   ├── items/item_catalog.hpp
│   ├── skills/（8 个，不变）
│   ├── grappling_hook/（2 个，不变）
│   ├── base_resource_sharing/
│   │   ├── ...（6 个，不变）
│   │   └── pal_base_resource_runtime.hpp ← 从 src/ 移入
│   ├── pal_identity/（2 个，不变）
│   ├── pal_stats/（2 个，不变）
│   └── mod/
│       └── mod_core.hpp                 ← 新：PalworldEditorMod 类声明（从 dllmain.cpp 提取）
├── src/
│   ├── mod/
│   │   ├── dllmain.cpp                  ← 瘦身：生命周期 + 回调 + game_thread_tick + 入口
│   │   └── render_dispatcher.cpp        ← ImGui 页签注册 + 各功能 render 分派
│   ├── items/
│   │   ├── item_ui.cpp                  ← render_give_items / item_browser / inventory
│   │   └── game_item_access.cpp         ← give_items / set_slot_count / scan_all_items
│   ├── skills/
│   │   ├── pal_skills.cpp
│   │   └── skill_ui.cpp                 ← render_passive_skills / active_skills / pal_editor
│   ├── grappling_hook/grapple_cooldown_gateway.cpp
│   ├── base_resource_sharing/
│   │   ├── pal_base_resource_runtime.cpp
│   │   ├── pal_base_resources.cpp
│   │   └── base_resource_ui.cpp         ← render_base_resource_sharing / render_grapple_no_cooldown
│   ├── pal_identity/pal_identity.cpp
│   └── pal_stats/
│       ├── pal_stats.cpp
│       └── stat_ui.cpp                  ← render_pal_stats
└── CMakeLists.txt                       ← 更新源文件列表
```

## 关键决策

### 1. `common/` 提取

从 `game/pal_game.hpp` 提取**真正跨功能共用**的反射原语到 `inc/common/game_reflection.hpp`：
- `is_valid(UObject*)`
- `invoke_object_return(UObject*, functionName)`
- `invoke_bool_return(UObject*, functionName)`
- `kInventoryClassName`

`text_encoding.hpp` 从 `support/` 移到 `common/`。

`pal_game.hpp` 中**功能特化**的函数（`give_items`、`set_slot_count`、`scan_all_items`、`read_inventory`、`resolve_selected_otomo`、`resolve_local_otomo_holder`、`discover_objects`）移到各自功能模块（items/skills）。

### 2. `mod_core.hpp` 提取

把 `PalworldEditorMod` 类声明（含所有成员变量、嵌套类型 `SkillEditorSnapshot` 等、static render 函数声明）从 `dllmain.cpp` 提取到 `inc/mod/mod_core.hpp`。这是拆分 render 实现的前提——各 `*_ui.cpp` 需要 include 类声明才能实现 static 成员函数。

### 3. `dllmain.cpp` 拆分

- render 函数的实现移到各功能的 `*_ui.cpp`（它们是 `PalworldEditorMod` 的 static 成员，合法访问私有成员）。
- `dllmain.cpp` 保留：构造函数（页签注册）、析构、`on_program_start`、`on_unreal_init`、`game_thread_tick`、`begin_world_transition`、`finish_world_transition`、`publish_skill_snapshot_if_dirty`、DLL 导出入口。
- `render_dispatcher.cpp` 可选：如果页签注册 lambda 太大，单独抽出。

### 4. `src/` 按功能分文件夹

每个 `.cpp` 放到对应功能子目录（`src/items/`、`src/skills/` 等）。`CMakeLists.txt` 更新源文件列表（显式列出或 `file(GLOB)` 各子目录）。

## 迁移策略（分阶段，每阶段构建通过）

### Phase 1：文件归位（低风险，机械操作）
- 移 `src/pal_base_resource_runtime.hpp` → `inc/base_resource_sharing/`。
- `src/` 按功能建子目录，移动 `.cpp`。
- 更新 `CMakeLists.txt` 源列表。
- 更新 `#include` 路径（`pal_base_resource_runtime.hpp` 路径变了）。
- **验证**：构建 + ctest 全通过。

### Phase 2：提取 `common/`（中风险）
- 创建 `inc/common/game_reflection.hpp`，移入 `is_valid` / `invoke_*` / `kInventoryClassName`。
- 移 `support/text_encoding.hpp` → `common/text_encoding.hpp`。
- 移 `pal_game.hpp` 的功能函数到 `src/items/`、`src/skills/`。
- 更新所有 `#include`（`<game/pal_game.hpp>` → `<common/game_reflection.hpp>` + 功能头）。
- **验证**：构建 + ctest 全通过。

### Phase 3：提取 `mod_core.hpp` + 拆 dllmain（高风险）
- 提取 `PalworldEditorMod` 类声明 → `inc/mod/mod_core.hpp`。
- 移 render 实现到各 `*_ui.cpp`。
- 瘦身 `dllmain.cpp`。
- **验证**：构建 + ctest + format-check 全通过。

每个 Phase 是一次独立提交（或几个小提交），构建必须通过才进入下一 Phase。

## 风险

- **Phase 3 风险最高**：`PalworldEditorMod` 类声明牵涉 50+ 成员和嵌套类型；提取到头文件后所有依赖它的 `.cpp` 都受影响。
- **include 路径大量变更**：每个 Phase 都需要批量更新 include 路径；遗漏会导致编译失败（容易发现）。
- **不改逻辑**：本重构不改变任何运行时行为；功能回归仍需游戏内验证（但不应产生新的 bug）。
