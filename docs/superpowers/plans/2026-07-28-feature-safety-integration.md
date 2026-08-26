# PalworldEditor 1.6.5–1.6.7 Safety Integration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 安全修复并累计集成被动技能分类、帕鲁属性编辑和爪钩枪无冷却。

**Architecture:** 纯 C++ 领域模型负责筛选、差量草稿、请求背压和可逆覆盖决策；Palworld 网关只在
EngineTick 内执行反射。三个既有分支依次合并到累计分支，冲突按最终模块边界统一解决。

**Tech Stack:** C++23、CMake、Ninja、MSVC、UE4SS、ImGui、CTest。

## Global Constraints

- 不跨 EngineTick 保存 Unreal 对象、函数、属性或结构地址。
- 默认关闭功能零扫描、零写入。
- 不增加空闲逐帧解析或全局扫描。
- GUI 只传递标准库值；Unreal 访问只在游戏线程。
- 每个行为修改执行 RED → GREEN → REFACTOR。
- 保留 `de49fff` 及三个功能分支的既有提交历史。

---

### Task 1: 集成并优化被动技能分类

**Files:**
- Modify: `mods/PalworldEditor/inc/skills/skill_catalog.hpp`
- Modify: `mods/PalworldEditor/src/dllmain.cpp`
- Modify: `mods/PalworldEditor/tests/skill_editor_tests.cpp`

**Interfaces:**
- Produces: `filter_passive_skill_views(...) -> std::vector<const SkillOption*>`
- Preserves: `PassiveSkillClassificationJob` 和运行时元数据批次接口

- [x] **Step 1: 合并 `codex/passive-skill-categories` 到累计分支**

运行 `git merge --no-ff codex/passive-skill-categories`，保留 v1.6.5 功能提交。

- [x] **Step 2: 写入失败测试**

新增测试，断言过滤结果保存原目录元素地址、保持顺序，并组合类别、搜索和排除条件。

- [x] **Step 3: 运行测试并确认 RED**

构建并运行 `PalworldEditorTests`，预期因 `filter_passive_skill_views` 尚不存在而编译失败。

- [x] **Step 4: 实现轻量过滤视图**

新增：

```cpp
[[nodiscard]] auto filter_passive_skill_views(
    std::span<const SkillOption> options,
    std::optional<PassiveSkillCategory> category,
    std::string_view query,
    const std::unordered_set<std::string>& excludedIds)
    -> std::vector<const SkillOption*>;
```

实现中 `reserve(options.size())`，只追加 `std::addressof(option)`。GUI 遍历指针，不复制 `SkillOption`。

- [x] **Step 5: 运行测试并确认 GREEN**

运行技能测试和格式检查。

- [x] **Step 6: 修复文档末尾空行并提交**

运行 `git diff --check`，提交 `fix: make passive category filtering allocation-light`。

### Task 2: 集成属性编辑并建立安全草稿

**Files:**
- Modify: `mods/PalworldEditor/inc/pal_stats/pal_stat_editor.hpp`
- Modify: `mods/PalworldEditor/src/dllmain.cpp`
- Modify: `mods/PalworldEditor/tests/skill_editor_tests.cpp`

**Interfaces:**
- Produces: `PalStatEditDraft::synchronize(...)`
- Produces: `PalStatEditDraft::make_request(...)`
- Produces: latest-wins `PalStatEditRequestSlot`

- [x] **Step 1: 合并 `codex/pal-stat-editor` 并解决冲突**

保留累计分类代码和 `de49fff` 的属性请求解析修复；版本更新为 1.6.6。

- [x] **Step 2: 写入草稿和请求槽失败测试**

测试从实际快照初始化、未修改时无请求、只改一个字段时其他字段为空、第二个请求覆盖第一个请求。

- [x] **Step 3: 运行测试并确认 RED**

预期因 `PalStatEditDraft` 和 `PalStatEditRequestSlot` 尚不存在而失败。

- [x] **Step 4: 实现草稿和 latest-wins 请求槽**

草稿保存原始/编辑快照和目标代数。生成请求时逐字段比较。请求槽使用
`std::mutex + std::optional<PalStatEditRequest>`，`submit()` 覆盖旧请求。

- [x] **Step 5: 接入 GUI 和 pending 状态**

目标快照可读时同步草稿；ImGui 输入直接编辑草稿；提交只发送差量请求。pending 同时包含技能、属性和选择请求。

- [x] **Step 6: 运行测试并确认 GREEN**

运行技能测试。

### Task 3: 统一属性目标授权

**Files:**
- Modify: `mods/PalworldEditor/inc/skills/selected_target_state.hpp`
- Modify: `mods/PalworldEditor/src/dllmain.cpp`
- Modify: `mods/PalworldEditor/tests/skill_editor_tests.cpp`

**Interfaces:**
- Produces: `bound_target_request_is_current(...) -> bool`

- [x] **Step 1: 写入错误目标失败测试**

构造锁定观察 A、当前观察 B、相同世界/目标代数，断言属性请求授权失败；A/A 时成功。

- [x] **Step 2: 运行测试并确认 RED**

预期因通用授权函数尚不存在而失败。

- [x] **Step 3: 实现通用授权函数**

函数验证世界可访问、世界确认、世界代数、目标代数、观察有效、锁定 GUID 匹配及非零目标句柄。

- [x] **Step 4: 属性路径改用统一授权**

只有授权成功才调用属性网关；失败清空待处理请求并发布拒绝消息。

- [x] **Step 5: 运行测试并确认 GREEN**

运行技能测试。

### Task 4: 属性网关预检和结构化结果

**Files:**
- Modify: `mods/PalworldEditor/inc/pal_stats/pal_stats.hpp`
- Modify: `mods/PalworldEditor/src/pal_stats.cpp`
- Modify: `mods/PalworldEditor/inc/pal_stats/pal_stat_editor.hpp`
- Modify: `mods/PalworldEditor/src/dllmain.cpp`
- Modify: `mods/PalworldEditor/tests/skill_editor_tests.cpp`

**Interfaces:**
- Produces: `PalStatEditStatus`
- Produces: `PalStatEditResult`
- Produces: pure `verify_stat_edit(...)`

- [x] **Step 1: 写入纯值验证失败测试**

测试请求字段全部匹配时成功、任一字段不匹配时失败、未请求字段不参与验证。

- [x] **Step 2: 运行测试并确认 RED**

预期因验证接口尚不存在而失败。

- [x] **Step 3: 实现纯值验证和结构化结果**

`PalStatEditResult` 保存状态、重读快照和消息；个体值普通上限改为 100。

- [x] **Step 4: 网关执行完整预检**

在任何写入前解析全部待写字段。亲密度阈值查询改为 `std::optional<int>`，失败时零写入。

- [x] **Step 5: 写后重读验证**

调用方根据请求字段验证重读值，发布明确成功/失败消息。无法安全恢复的 Unreal 副作用不伪装为成功。

- [x] **Step 6: 运行测试并确认 GREEN**

运行技能测试、格式检查和完整构建。

### Task 5: 集成并重构爪钩覆盖服务

**Files:**
- Create: `mods/PalworldEditor/inc/grappling_hook/cooldown_service.hpp`
- Create: `mods/PalworldEditor/inc/grappling_hook/settings.hpp`
- Create: `mods/PalworldEditor/src/grapple_cooldown_gateway.cpp`
- Modify: `mods/PalworldEditor/inc/game/pal_game.hpp`
- Modify: `mods/PalworldEditor/src/dllmain.cpp`
- Modify: `mods/PalworldEditor/src/base_resource_settings.cpp`
- Modify: `mods/PalworldEditor/CMakeLists.txt`
- Modify: `mods/PalworldEditor/tests/base_resource_sharing_tests.cpp`

**Interfaces:**
- Produces: pure `CooldownOverrideLedger`
- Produces: `GrappleCooldownGateway::apply()` 和 `restore()`

- [x] **Step 1: 合并 `codex/grapple-no-cooldown` 并解决冲突**

保留累计分类和属性编辑，版本更新为 1.6.7。

- [x] **Step 2: 写入默认关闭和恢复账本失败测试**

测试默认关闭不请求扫描；开启记录每个目标原值；重复开启幂等；关闭按各自原值恢复；世界开始清空已恢复账本。

- [x] **Step 3: 运行测试并确认 RED**

预期因独立爪钩服务尚不存在而失败。

- [x] **Step 4: 实现纯值账本和状态机**

账本只保存对象全名、原值和世界代次，不保存 Unreal 指针。默认关闭且账本为空时返回 no-op。

- [x] **Step 5: 实现严格目标网关**

删除 `set_grapple_no_cooldown(bool)` 对全部 `PalWeaponBase` 的写入。候选必须通过明确爪钩识别；无法确认时返回
`targetUnavailable` 且零写入。恢复只能使用账本原值。

- [x] **Step 6: 接入启动、切换和 LoadMap**

启动关闭不设置 dirty；LoadMap 前恢复；开启状态在世界就绪后提交一次应用。C++ mod 析构不保证位于游戏线程，
因此析构不访问 Unreal；热卸载前必须先通过 GUI 关闭开关。
> **已过时（superseded）**：卸载协议已改为卸载线程请求游戏线程有界清理（立即一次、失败后每 2 秒、
> 最多 5 次），无需用户预先关闭开关；见 AGENTS.md"验证一次改动"的热重载段。

- [x] **Step 7: 运行测试并确认 GREEN**

运行配置/资源测试、格式检查和完整构建。

### Task 6: 累计验证与文档同步

**Files:**
- Modify: `README.md`
- Modify: `AGENTS.md`
- Modify: `CLAUDE.md`

- [x] **Step 1: 同步最终 1.6.7 架构和安全契约**

记录分类轻量过滤、属性差量/目标授权、爪钩默认关闭 no-op 和可逆恢复。

- [x] **Step 2: 运行完整验证**

```powershell
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor PalworldEditorTests PalworldEditorBaseResourceSharingTests
ctest --test-dir build --output-on-failure
git diff --check
```

- [x] **Step 3: 检查提交拓扑与工作树**

确认累计分支包含三个功能历史、`de49fff` 可达、无未提交文件。

- [x] **Step 4: 输出游戏内验证清单**

要求验证错误目标拒绝、属性差量、默认关闭零副作用、爪钩原值恢复和帧时间。
