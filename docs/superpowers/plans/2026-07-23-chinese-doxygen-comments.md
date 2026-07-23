# PalworldEditor 中文 Doxygen 注释实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为 `PalworldEditor` 的 8 个生产代码文件补齐详细、准确、可由 Doxygen 消费的中文接口与字段注释，同时保持 C++ 行为完全不变。

**Architecture:** 按现有模块边界分批注释：先处理无 Unreal 依赖的纯值逻辑，再处理技能编辑服务、游戏反射适配、技能网关，最后处理负责线程协调与 ImGui 的 mod 入口。公共声明写完整契约，源文件定义通过 `@copydoc` 继承契约并补充反射实现细节；最终以逐符号覆盖审计和现有构建链证明没有引入行为变化。

**Tech Stack:** C++23、UE4SS、Unreal 反射、ImGui、Doxygen 注释语法、CMake、Ninja、CTest、clang-format、clang-tidy。

## Global Constraints

- 只修改 `mods/PalworldEditor/inc` 和 `mods/PalworldEditor/src` 下的 8 个生产代码文件。
- 不修改 `mods/PalworldEditor/tests/skill_editor_tests.cpp` 和第三方 `RE-UE4SS`。
- 不添加 `Doxyfile`，不生成 HTML、XML 或其他文档产物。
- 不修改标识符、类型、函数签名、默认参数、include、控制流、字段顺序、初始化值、日志文字、UI 文案或反射路径。
- 每个文件、命名空间、类型、枚举值、函数、方法、构造函数、回调、导出接口、常量和字段都必须有中文 Doxygen 注释。
- 公共接口使用完整的 `@brief`、`@param[in]`、`@param[out]`、`@return`、`@retval`、`@warning` 和 `@note` 契约；只使用实际适用的标签。
- 裸 `UObject*`/`UFunction*` 明确标注为非拥有观察指针，并说明 `nullptr` 和游戏对象生命周期。
- 涉及线程共享的数据明确标注生产者、消费者、保护互斥量或原子状态用途。
- 本任务不新增测试；每批改动通过现有编译和测试验证注释没有破坏代码。

---

### Task 1: 注释文本编码与纯目录逻辑

**Files:**
- Modify: `mods/PalworldEditor/inc/support/text_encoding.hpp:1-48`
- Modify: `mods/PalworldEditor/inc/items/item_catalog.hpp:1-97`
- Modify: `mods/PalworldEditor/inc/skills/skill_catalog.hpp:1-91`
- Test: `mods/PalworldEditor/tests/skill_editor_tests.cpp`

**Interfaces:**
- Consumes: Windows `WideCharToMultiByte`、`std::string_view`、目录值类型和现有搜索算法。
- Produces: 带完整中文契约的 `text_encoding`、`item_catalog` 和 `skill_editor` 纯逻辑接口；不改变任何签名或返回值。

- [ ] **Step 1: 为三个头文件添加文件和命名空间注释**

在每个 `#pragma once` 前添加 `@file` 注释，并在命名空间声明前说明模块边界。格式固定为：

```cpp
/**
 * @file item_catalog.hpp
 * @brief 提供与 Unreal 运行时无关的物品目录整理、标签生成和搜索能力。
 * @details 本文件只处理 UTF-8 字符串和值类型，不持有任何游戏对象。
 */
```

`text_encoding` 必须说明 Windows UTF-16/UTF-8 转换边界；`skill_editor` 必须说明本文件只负责技能目录展示和筛选，不执行游戏写入。

- [ ] **Step 2: 注释 `text_encoding.hpp` 的全部接口**

为 `to_utf8(std::wstring_view)` 写明：

```cpp
/**
 * @brief 将 UTF-16 宽字符串严格转换为 UTF-8。
 * @param[in] value 待转换的 UTF-16 文本视图；函数不保存该视图。
 * @return 转换后的 UTF-8 字符串。
 * @retval std::string{} 输入为空、长度超过 Win32 `int` 上限、包含非法 UTF-16，
 *         或 `WideCharToMultiByte` 未完整写入时返回空字符串。
 */
```

为 `widen_ascii(std::string_view)` 明确它逐字节扩展 Raw ID，只适用于 ASCII 标识符，不是通用 UTF-8 转 UTF-16 接口。

- [ ] **Step 3: 注释 `item_catalog.hpp` 的全部类型、字段和函数**

覆盖 `ItemOption`、`ItemCatalogSnapshot` 的每个字段，以及以下函数：

```cpp
ascii_lower(std::string_view)
item_label(const ItemOption&)
item_label(const ItemCatalogSnapshot&, std::string_view)
matches_item(const ItemOption&, std::string_view)
filter_items(const ItemCatalogSnapshot&, std::string_view)
make_item_catalog(std::vector<ItemOption>)
```

字段契约必须明确：

```cpp
std::string id;            /**< 写入 Palworld 接口的物品 Raw ID。 */
std::string localizedName; /**< 当前游戏语言的展示名称；为空时界面回退到 `id`。 */
```

`filter_items` 的返回指针必须注明非拥有、仅在 `catalog.items` 未发生修改或析构时有效。`make_item_catalog` 必须说明忽略空 ID、按 ID 去重、优先保留非空本地化名、按最终标签稳定排序并生成 `labelsById`。

- [ ] **Step 4: 注释 `skill_catalog.hpp` 的全部类型、字段和函数**

覆盖 `SkillOption`、`SkillCatalogSnapshot` 的每个字段，以及：

```cpp
with_catalog_fallback(...)
ascii_lower(...)
skill_label(...)
matches_skill(...)
deduplicate_skills(...)
filter_skills(...)
```

`activeValue` 注明仅主动技能具有 `EPalWazaID` 数值；`ready` 注明真假各自含义；`with_catalog_fallback` 注明刷新失败时保留上一份可用目录但传播最新错误；`filter_skills` 注明结果是值拷贝且同时应用搜索和排除集合。

- [ ] **Step 5: 运行纯逻辑测试与格式检查**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditorTests
ctest --test-dir build --output-on-failure
```

Expected: `format-check` 返回 0；`PalworldEditorTests` 构建成功；CTest 显示 `1/1` 通过。

- [ ] **Step 6: 提交纯逻辑注释**

```powershell
git add mods/PalworldEditor/inc/support/text_encoding.hpp `
        mods/PalworldEditor/inc/items/item_catalog.hpp `
        mods/PalworldEditor/inc/skills/skill_catalog.hpp
git commit -m "docs: annotate catalog utilities"
```

### Task 2: 注释技能编辑领域模型与服务

**Files:**
- Modify: `mods/PalworldEditor/inc/skills/skill_editor_service.hpp:1-283`
- Test: `mods/PalworldEditor/tests/skill_editor_tests.cpp`

**Interfaces:**
- Consumes: Task 1 中已注释的 `skill_editor` 命名空间约定。
- Produces: 技能请求、结果、线程安全队列、游戏网关抽象和执行/回滚算法的完整中文契约。

- [ ] **Step 1: 注释领域类型、别名、枚举和值语义**

覆盖：

```cpp
SkillTarget
ActiveSkill
SkillState
SkillKind
SkillEditOperation
SkillEditRequest
SkillEditStatus
SkillEditResult
```

每个枚举值使用 `/**< ... */` 说明精确状态。`SkillTarget` 明确是把非拥有 `UObject*` 编码为整数后的临时句柄，使用前必须由 `ISkillGateway::is_valid` 校验。`SkillEditRequest` 的每个字段说明在哪种 `kind`/`operation` 组合下生效。`SkillEditResult::state` 说明它是操作或回滚后重新读取的实际游戏状态。

- [ ] **Step 2: 注释 `SkillEditQueue` 的接口和字段**

类注释必须说明多生产者/单游戏线程消费者模型和 FIFO 保证。覆盖 `push`、`try_pop`、`size`、`contains_target`，并明确所有方法内部加锁。

字段采用以下语义：

```cpp
mutable std::mutex mutex_;              /**< 保护 `requests_` 的唯一互斥量。 */
std::deque<SkillEditRequest> requests_; /**< 按提交顺序保存尚未由游戏线程执行的请求。 */
```

- [ ] **Step 3: 注释 `ISkillGateway` 的完整抽象契约**

覆盖虚析构和五个纯虚函数。每个 `SkillTarget` 参数注明调用前仍需校验；`read_state` 说明返回游戏实际值；`add_passive`、`remove_passive` 和 `rewrite_active` 的布尔值只表示反射调用能否发起，不代表游戏一定接受修改，调用方必须重读验证。

`rewrite_active` 还要说明：

- 输入顺序就是 `EquipWaza` 槽位顺序；
- 最多 3 项；
- 实现可能先清空再逐项重写。

- [ ] **Step 4: 注释 `detail` 中的全部辅助算法**

覆盖：

```cpp
contains_passive(...)
same_passives(...)
result(...)
execute_passive(...)
contains_active(...)
same_active_sequence(...)
execute_active(...)
```

`same_passives` 说明忽略顺序但保留重复次数；`same_active_sequence` 说明只按数值和顺序验证；两个 `execute_*` 说明验证、写入、重读、失败回滚和 `rollbackFailed` 的完整状态机。

- [ ] **Step 5: 注释统一入口 `execute_skill_edit`**

说明它先拒绝空/失效目标，再读取原始状态并按 `SkillKind` 分派；返回值始终包含可获得的最新实际状态和面向 UI 的消息。添加 `@warning`，要求网关实现涉及 Unreal 反射时必须在游戏线程调用。

- [ ] **Step 6: 运行技能服务测试**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditorTests
ctest --test-dir build --output-on-failure
```

Expected: 构建返回 0，CTest 显示 `100% tests passed`。

- [ ] **Step 7: 提交技能服务注释**

```powershell
git add mods/PalworldEditor/inc/skills/skill_editor_service.hpp
git commit -m "docs: annotate skill edit service"
```

### Task 3: 注释 Palworld 游戏反射适配层

**Files:**
- Modify: `mods/PalworldEditor/inc/game/pal_game.hpp:1-336`
- Test: `mods/PalworldEditor/tests/skill_editor_tests.cpp`

**Interfaces:**
- Consumes: Task 1 的 `item_catalog`、`text_encoding`，以及 UE4SS `UObjectGlobals`/`ProcessEvent`。
- Produces: 背包、物品、本地化、帕鲁扫描和诊断接口的线程、所有权与失败回退契约。

- [ ] **Step 1: 把现有英文文件说明改为 Doxygen 文件注释并注释命名空间**

文件注释必须明确：所有函数只能在 `on_unreal_init()` 之后的 Unreal 游戏线程调用；返回的 UObject 裸指针不转移所有权；反射函数或对象缺失时采用空值回退。

- [ ] **Step 2: 注释常量、基础校验和数据结构**

覆盖：

```cpp
kInventoryClassName
is_valid(UObject*)
kDiscoveryKeywords
InvEntry::{item_id,count,slot_index}
PalEntry::{name,ptr}
```

`is_valid` 明确它只做轻量陈旧指针检查，不能延长对象生命周期。`PalEntry::ptr` 明确非拥有，跨帧使用前必须重新校验。`InvEntry::slot_index` 明确修改数量时使用槽位而不是物品 ID。

- [ ] **Step 3: 注释背包读取和写入接口**

覆盖：

```cpp
get_main_container()
container_num(UObject*)
container_get(UObject*, int32_t)
read_slot_stack_count(UObject*)
read_inventory()
set_slot_count(int32_t, int32_t)
give_items(const std::string&, int32)
```

每个返回 `UObject*` 的函数标注非拥有和 `nullptr` 条件。`container_get` 说明索引由调用方保证处于容器范围内。`set_slot_count` 说明直接写 `StackCount` 且不做范围裁剪。`give_items` 说明 `itemId` 必须是 ASCII Raw ID，数量直接传给 `AddItem_ServerInternal`。

- [ ] **Step 4: 注释本地化和物品扫描接口**

覆盖：

```cpp
get_ui_utility()
localized_item_name(UObject*, UFunction*, UObject*, const FName&)
scan_all_items()
```

`localized_item_name` 的四个参数分别注明非拥有关系和用途；返回空字符串表示不能解析名称。`scan_all_items` 说明仅扫描当前已加载的 `PalStaticItemData*` UObject，过滤表/资产/管理类，从 `ID` 读取 Raw ID，并通过 `PalUIUtility:GetItemName` 获取当前游戏语言名称。

- [ ] **Step 5: 注释帕鲁扫描和诊断接口**

`scan_pals` 说明只收集已加载的 `PalIndividualCharacterParameter`，名称附加 `[boxed]`/`[active]` 状态，返回指针仍依赖游戏对象生命周期。`discover_objects` 说明它只记录命中 `kDiscoveryKeywords` 的类名直方图，并限制最多输出 200 类。

- [ ] **Step 6: 构建 mod 并检查格式**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor
```

Expected: 两个目标均返回 0，生成 `build/Game__Shipping__Win64/bin/PalworldEditor.dll`。

- [ ] **Step 7: 提交游戏适配层注释**

```powershell
git add mods/PalworldEditor/inc/game/pal_game.hpp
git commit -m "docs: annotate game reflection helpers"
```

### Task 4: 注释 Pal 技能反射网关

**Files:**
- Modify: `mods/PalworldEditor/inc/skills/pal_skills.hpp:1-24`
- Modify: `mods/PalworldEditor/src/pal_skills.cpp:1-277`
- Test: `mods/PalworldEditor/tests/skill_editor_tests.cpp`

**Interfaces:**
- Consumes: Task 2 的 `ISkillGateway` 契约和 Task 3 的 `pal_game::is_valid`。
- Produces: `PalSkillGateway` 公共接口、主动技能数值映射和全部反射辅助函数的中文注释。

- [ ] **Step 1: 注释头文件中的类、覆盖接口和缓存字段**

为 `PalSkillGateway` 说明它把纯领域服务适配到 `PalIndividualCharacterParameter` 反射 API，且所有方法必须在游戏线程调用。为六个接口写完整参数和返回契约。

`activeIds_` 必须说明它由最近一次成功的 `load_catalog` 重建，用于把 `EPalWazaID` 数值还原为 Raw ID，不拥有任何 Unreal 对象。

- [ ] **Step 2: 注释源文件的匿名命名空间及全部辅助符号**

覆盖：

```cpp
EPalWazaID
to_pal(SkillTarget)
find_function<T>(const wchar_t*)
ui_utility()
passive_localized_name(...)
active_localized_name(...)
strip_enum_prefix(std::string)
is_active_sentinel(std::string_view)
```

模板 `find_function` 明确返回非拥有指针；两个本地化函数说明当前语言 `FText` 转 UTF-8 的空值回退；`is_active_sentinel` 列明过滤 `None`、`Max` 和 `_MAX` 尾项。

- [ ] **Step 3: 在六个成员函数定义上使用 `@copydoc` 并补充实现细节**

定义前采用以下模式：

```cpp
/**
 * @copydoc PalSkillGateway::read_state
 * @details 被动技能通过 `GetPassiveSkillList` 读取；主动技能通过
 * `GetEquipWaza` 读取，并限制为可编辑的前三个槽位。
 */
```

分别补充：

- `is_valid`：调用 `pal_game::is_valid`；
- `add_passive`/`remove_passive`：构造与 UHT 参数布局一致的临时参数；
- `rewrite_active`：先 `ClearEquipWaza` 再按输入顺序 `AddEquipWaza`；
- `load_catalog`：从 `PalPassiveSkillManager` 和 `EPalWazaID` 反射枚举加载、去重、过滤哨兵值、排序并重建 `activeIds_`。

- [ ] **Step 4: 构建并运行测试**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor PalworldEditorTests
ctest --test-dir build --output-on-failure
```

Expected: 构建返回 0，CTest 显示 `1/1` 通过。

- [ ] **Step 5: 提交技能网关注释**

```powershell
git add mods/PalworldEditor/inc/skills/pal_skills.hpp `
        mods/PalworldEditor/src/pal_skills.cpp
git commit -m "docs: annotate Pal skill gateway"
```

### Task 5: 注释 mod 生命周期、线程协调和 ImGui

**Files:**
- Modify: `mods/PalworldEditor/src/dllmain.cpp:1-741`
- Test: `mods/PalworldEditor/tests/skill_editor_tests.cpp`

**Interfaces:**
- Consumes: Tasks 1-4 的已注释目录、游戏适配和技能网关接口。
- Produces: `PalworldEditorMod`、嵌套状态类型、RAII 守卫、全部 UI 辅助函数、每个成员字段及 DLL 导出接口的中文 Doxygen 注释。

- [ ] **Step 1: 把文件头改为 Doxygen 文档**

保留现有构建、部署、职责和线程模型信息，但改成 `@file`、`@brief`、`@details`。明确 ImGui 回调只提交请求，`on_update` 在游戏线程执行反射，结果通过互斥量保护的快照返回 GUI。

- [ ] **Step 2: 注释 `PalworldEditorMod` 生命周期接口**

覆盖类、构造函数、析构函数、`on_unreal_init` 和 `on_update`：

- 构造函数设置元数据并注册 ImGui 页签，不访问 Unreal 对象；
- `on_unreal_init` 在 UE4SS 完成 Unreal 初始化后注册 `ProcessEvent` 前置回调；
- `on_update` 是唯一消费物品、背包、扫描和技能编辑请求的游戏线程入口；
- 析构函数不拥有 Hook 回调之外的 Unreal 资源。

- [ ] **Step 3: 注释目标解析类型和 RAII 守卫**

覆盖：

```cpp
SkillTargetSource::{none,viewed,selected}
ResolvedSkillTarget::{target,source,name}
SkillEditorSnapshot::{target,source,palName,state,catalog,lastResult,pending}
ViewTrackingGuard
ViewTrackingGuard::flag_
```

`ViewTrackingGuard` 说明构造时抑制由本 mod 自己触发的 `GetPassiveSkillList` 追踪，析构时恢复；引用字段不拥有原子变量且守卫生命周期不得超过 mod。

- [ ] **Step 4: 注释全部目标解析和 UI 辅助函数**

覆盖：

```cpp
clamp(...)
resolve_skill_target()
find_skill_label(...)
reset_skill_editor_ui(...)
render_skill_picker(...)
render_give_items(...)
render_item_browser(...)
render_inventory(...)
render_passive_skills(...)
render_active_skills(...)
render_pal_editor(...)
```

每个 `PalworldEditorMod* self` 标注为非拥有且不得为空。渲染函数明确只读取缓存并提交原子/队列请求，不调用 Unreal 反射。`resolve_skill_target` 说明优先当前查看对象，失败后回退手动选择。物品浏览器明确显示本地化标签但只复制 Raw ID；背包修改明确提交 `slot_index`。

- [ ] **Step 5: 注释每个成员字段**

按以下同步组逐字段注释：

- GUI 线程独占：`item_buf_`、`search_buf_`、`count_input_`、`set_count_input_`、`selected_`；
- `req_mutex_` 保护的给予/修改参数和对应原子请求标志；
- `inv_mutex_` 保护的背包、物品目录、帕鲁列表和查看对象名称；
- 扫描/刷新原子请求标志；
- `GetPassiveSkillList` Hook 追踪状态和非拥有 `fnGetPSL_`；
- 技能网关、FIFO 队列、`skillSnapshotMutex_` 与技能快照；
- GUI 线程独占的技能搜索缓冲区、编辑索引、选择项和当前 UI 目标。

`passiveEditIndex_` 明确 `-1` 表示未编辑、`-2` 表示新增、非负值表示替换索引；`activeEditSlot_` 明确 `-1` 表示未编辑。

- [ ] **Step 6: 注释 DLL ABI 宏和导出接口**

为 `PALWORLD_EDITOR_API` 说明 Windows DLL 导出用途。为：

```cpp
extern "C" CppUserModBase* start_mod()
extern "C" void uninstall_mod(CppUserModBase* mod)
```

说明 C ABI、所有权转移和成对调用要求：`start_mod` 把新实例所有权交给 UE4SS，`uninstall_mod` 接收并销毁该实例，`mod` 必须来自本 DLL 的 `start_mod`。

- [ ] **Step 7: 构建完整目标并运行测试**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor PalworldEditorTests
ctest --test-dir build --output-on-failure
```

Expected: 格式检查和构建返回 0；CTest 显示 `100% tests passed`。

- [ ] **Step 8: 提交 mod 入口注释**

```powershell
git add mods/PalworldEditor/src/dllmain.cpp
git commit -m "docs: annotate mod lifecycle and UI"
```

### Task 6: 全量覆盖审计和最终验证

**Files:**
- Inspect: `mods/PalworldEditor/inc/**/*.hpp`
- Inspect: `mods/PalworldEditor/src/*.cpp`
- Exclude: `mods/PalworldEditor/tests/skill_editor_tests.cpp`

**Interfaces:**
- Consumes: Tasks 1-5 的全部注释提交。
- Produces: 逐文件覆盖证据、无行为改动证据和完整工具链验证结果。

- [ ] **Step 1: 逐符号检查 Doxygen 覆盖**

Run:

```powershell
rg -n "^\\s*(class|struct|enum class|using |inline |static |auto |PALWORLD_EDITOR_API)" `
   mods/PalworldEditor/inc mods/PalworldEditor/src
rg -n "/\\*\\*|@file|@brief|@param|@return|@copydoc|/\\*\\*<" `
   mods/PalworldEditor/inc mods/PalworldEditor/src
```

Expected: 8 个文件均含 `@file`；每个类型、枚举值、函数和字段在声明前或声明尾部紧邻 Doxygen 注释。对第一条命令列出的每个生产符号进行人工一一对应核对。

- [ ] **Step 2: 确认修改边界**

Run:

```powershell
git diff 2f27fea..HEAD --name-only -- mods/PalworldEditor/tests RE-UE4SS
git diff --check
```

Expected: 第一条命令没有本任务新增的测试或第三方文件；第二条命令没有输出并返回 0。人工查看本任务各提交，确认 C++ token、签名、控制流、字段顺序和字面量均未改变。

- [ ] **Step 3: 运行完整格式、构建和测试验证**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target format-check
cmake --build --preset ninja-msvc-x64 --target PalworldEditor PalworldEditorTests
ctest --test-dir build --output-on-failure
```

Expected: 所有命令返回 0，CTest 显示 `1/1` 通过。

- [ ] **Step 4: 运行 clang-tidy**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target tidy-check
```

Expected: 命令返回 0；允许显示项目现有的建议级 warning，但不得出现新的编译错误或 clang-tidy 执行失败。

- [ ] **Step 5: 核对最终工作区**

Run:

```powershell
git status --short
git log -6 --oneline
```

Expected: 本任务的 5 个源码注释提交均已存在；工作区只保留用户在任务开始前已有的 `.gitignore`、`AGENTS.md` 和 `UHTHeaderDump.7z` 变更。
