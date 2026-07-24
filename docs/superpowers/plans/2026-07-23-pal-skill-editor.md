# 当前选中帕鲁主动/被动技能编辑器实施计划

> **供执行代理使用：** 必须使用 `superpowers:executing-plans` 子技能，严格按任务顺序实施；每个任务都先写失败测试，再写最小实现，最后提交。

**目标：** 将 MyPalMod 升级到 1.4.0，为当前查看或手动选中的帕鲁提供安全、可搜索、可回滚的被动技能与主动技能编辑能力。

**架构：** 把不依赖 Unreal 的目录筛选、编辑规则、回滚和 FIFO 队列放入可由 CTest 覆盖的纯 C++ 层；把 `TArray`、`FName`、`FText`、`ProcessEvent` 和 Palworld 反射对象限制在 UE 适配层；ImGui 只读取快照并投递携带目标地址的请求，所有 Unreal 调用继续由 `on_update()` 在游戏线程执行。

**技术栈：** C++23、UE4SS/Palworld 反射 API、Dear ImGui、CMake 3.22+、Ninja、CTest、MSVC。

**设计依据：** `docs/superpowers/specs/2026-07-23-pal-skill-editor-design.md`

---

## 实施约束

- 只编辑 `UPalIndividualCharacterParameter` 的被动技能和 `EquipWaza`；不得修改 `MasteredWaza`，也不实现伙伴技能编辑。
- 被动技能目录必须来自“帕鲁可分配”集合。不得把整个 `PassiveSkillDataTable` 未经过滤地展示给用户。
- 主动技能目录来自 `EPalWazaID` 反射枚举，排除空值、无效值和 `MAX` 哨兵。
- 不直接写 `SaveParameter`、`PassiveSkillList`、`EquipWaza` 或 DataTable 内存；只调用游戏公开函数。
- 所有返回数组必须使用 UE4SS 的真实 `TArray<T>` 管理生命周期；删除当前 `{Data, Num, Max}` 裸结构。
- ImGui 线程不得调用 Unreal API。请求在点击时捕获目标，由 `on_update()` 按 FIFO 顺序执行。
- 当前查看目标有效时优先；否则退回 “Scan Pals” 中的手动选择。
- 支持范围是单机和房主/本地主机；普通联机客户端不承诺生效。
- 工作区存在用户自己的 `.gitignore`、`AGENTS.md` 和 `UHTHeaderDump.7z` 改动。每次提交只 `git add` 本任务列出的文件，不得顺带提交这些文件。

## 计划中的核心类型

以下接口在后续任务中保持不变，避免测试、UI 和 UE 适配层各自发明一套类型：

```cpp
namespace skill_editor
{
using SkillTarget = std::uintptr_t;

struct ActiveSkill
{
    std::uint16_t value{};
    std::string id;
    friend auto operator==(const ActiveSkill&, const ActiveSkill&) -> bool = default;
};

struct SkillOption
{
    std::string id;
    std::string localizedName;
    std::optional<std::uint16_t> activeValue;
};

struct SkillState
{
    std::vector<std::string> passiveIds;
    std::vector<ActiveSkill> activeSkills; // 按 EquipWaza 顺序，最多 3 个
};

enum class SkillKind
{
    passive,
    active,
};

enum class SkillEditOperation
{
    add,
    replace,
    remove,
};

struct SkillEditRequest
{
    SkillTarget target{};
    SkillKind kind{};
    SkillEditOperation operation{};
    std::size_t activeSlot{};                    // 仅主动技能使用，范围 0..2
    std::string oldPassiveId;                    // 被动替换/删除时使用
    std::string newPassiveId;                    // 被动新增/替换时使用
    std::optional<ActiveSkill> newActiveSkill;   // 主动新增/替换时使用
};

enum class SkillEditStatus
{
    succeeded,
    invalidTarget,
    rejected,
    failed,
    rolledBack,
    rollbackFailed,
};

struct SkillEditResult
{
    SkillEditStatus status{};
    SkillState state;
    std::string message;
};
}
```

## 验证命令约定

所有构建命令都在 VS 2022 x64 Developer PowerShell 中运行：

```powershell
cmake --preset ninja-msvc-x64
cmake --build --preset ninja-msvc-x64 --target MyPalModSkillTests
ctest --test-dir build --output-on-failure
cmake --build --preset ninja-msvc-x64
```

预期最终结果：

- `MyPalModSkillTests` 构建成功；
- CTest 显示 `100% tests passed, 0 tests failed`；
- `MyPalMod.dll` 构建成功；
- 不出现新的编译警告或 clang-tidy 诊断；
- 游戏内端到端验收通过。

---

### 任务 1：建立纯 C++ 测试目标和烟雾测试

**文件：**

- 修改：`CMakeLists.txt`
- 修改：`mods/MyPalMod/CMakeLists.txt`
- 新建：`mods/MyPalMod/tests/skill_editor_tests.cpp`

**步骤 1：在根 CMake 开启 CTest**

在根 `project(...)` 后加入：

```cmake
include(CTest)
```

**步骤 2：添加不链接 UE4SS 的测试目标**

在 `mods/MyPalMod/CMakeLists.txt` 的部署配置之前加入：

```cmake
if(BUILD_TESTING)
    add_executable(MyPalModSkillTests
        tests/skill_editor_tests.cpp
    )
    target_include_directories(MyPalModSkillTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(MyPalModSkillTests PRIVATE cxx_std_23)
    add_test(NAME MyPalMod.SkillEditor COMMAND MyPalModSkillTests)
endif()
```

测试目标不能链接 `UE4SS`，以此保证被测头文件没有偷偷依赖 Unreal 类型。

**步骤 3：写最小测试运行器和烟雾测试**

`skill_editor_tests.cpp` 只使用标准库，提供 `CHECK(...)`、失败计数和 `main()`。先用 `CHECK(true)` 验证测试目标、
运行器和 CTest 注册都正确；此任务不引入尚不存在的生产头文件。

**步骤 4：运行测试并确认基础设施为 GREEN**

```powershell
cmake --preset ninja-msvc-x64
cmake --build --preset ninja-msvc-x64 --target MyPalModSkillTests
ctest --test-dir build --output-on-failure
```

预期：测试目标编译成功，CTest 显示一个烟雾测试通过。若失败原因是 CMake、MSVC 环境或第三方依赖，先修复测试基础设施，
再进入功能 TDD。

**步骤 5：提交测试脚手架**

```powershell
git add CMakeLists.txt mods/MyPalMod/CMakeLists.txt mods/MyPalMod/tests/skill_editor_tests.cpp
git commit -m "test: add skill editor CTest target"
```

---

### 任务 2：实现技能目录的显示、搜索和排除规则

**文件：**

- 新建：`mods/MyPalMod/src/skill_catalog.hpp`
- 修改：`mods/MyPalMod/tests/skill_editor_tests.cpp`

**步骤 1：补齐失败测试**

让测试包含尚不存在的 `skill_catalog.hpp`，并增加以下用例：

- 中文名精确/子串搜索；
- 原始 ID 大小写不敏感搜索；
- 本地化为空时显示原始 ID；
- 本地化存在时标签为 `中文名 [RawId]`；
- 被动下拉排除当前已拥有的 ID；
- 主动下拉排除当前已装备的枚举值；
- 同一目录中重复 ID 去重并保持首次出现顺序。

核心断言示例：

```cpp
const std::vector<skill_editor::SkillOption> options{
    {.id = "Passive_Swift", .localizedName = "神速"},
    {.id = "Passive_Workaholic", .localizedName = "工作狂"},
};

CHECK(skill_editor::matches_skill(options[0], "神速"));
CHECK(skill_editor::matches_skill(options[0], "passive_swift"));
CHECK(!skill_editor::matches_skill(options[1], "神速"));
CHECK(skill_editor::skill_label(options[0]) == "神速 [Passive_Swift]");
CHECK(skill_editor::skill_label({.id = "Passive_Unknown"}) == "Passive_Unknown");

const auto visible =
    skill_editor::filter_skills(options, "passive", std::unordered_set<std::string>{"Passive_Swift"});
CHECK(visible.size() == 1);
CHECK(visible[0].id == "Passive_Workaholic");
```

**步骤 2：运行测试并确认 RED**

预期：链接或编译失败，因为目录函数尚未实现。

**步骤 3：实现最小纯函数**

`skill_catalog.hpp` 实现：

```cpp
auto ascii_lower(std::string_view value) -> std::string;
auto skill_label(const SkillOption& option) -> std::string;
auto matches_skill(const SkillOption& option, std::string_view query) -> bool;
auto deduplicate_skills(std::vector<SkillOption> options) -> std::vector<SkillOption>;
auto filter_skills(
    std::span<const SkillOption> options,
    std::string_view query,
    const std::unordered_set<std::string>& excludedIds) -> std::vector<SkillOption>;
```

约束：

- 只对 ASCII 字节做小写化，UTF-8 中文按原字节进行子串匹配；
- 排除集合比较原始 ID，不比较本地化名称；
- 不在此层判断“是否可分配”，目录来源过滤由 UE 适配层负责。

**步骤 4：运行测试并确认 GREEN**

预期：目录相关测试全部通过。

**步骤 5：提交**

```powershell
git add mods/MyPalMod/src/skill_catalog.hpp mods/MyPalMod/tests/skill_editor_tests.cpp
git commit -m "feat: add searchable skill catalog model"
```

---

### 任务 3：测试并实现被动技能编辑、校验和回滚

**文件：**

- 新建：`mods/MyPalMod/src/skill_editor_service.hpp`
- 修改：`mods/MyPalMod/tests/skill_editor_tests.cpp`

**步骤 1：在测试中实现 `FakeSkillGateway`**

生产接口：

```cpp
class ISkillGateway
{
public:
    virtual ~ISkillGateway() = default;
    virtual auto is_valid(SkillTarget target) const -> bool = 0;
    virtual auto read_state(SkillTarget target) -> SkillState = 0;
    virtual auto add_passive(SkillTarget target, std::string_view id) -> bool = 0;
    virtual auto remove_passive(SkillTarget target, std::string_view id) -> bool = 0;
    virtual auto rewrite_active(
        SkillTarget target,
        std::span<const ActiveSkill> skills) -> bool = 0;
};
```

Fake 保存内存中的 `SkillState`，并提供 `failNextAddPassive`、`failNextRemovePassive`、
`failNextRewriteActive` 开关和调用日志。

**步骤 2：写被动技能失败测试**

至少覆盖：

1. 无效目标返回 `invalidTarget`，不调用任何写操作；
2. 已有 4 个被动时拒绝新增，返回 `rejected`；
3. 重复新增被拒绝；
4. 删除不存在的技能被拒绝；
5. 新增、删除成功后返回重新读取的实际状态；
6. 替换按“删除旧项 → 新增新项 → 重读确认”执行；
7. 替换后新技能未出现时，删除可能残留的新项并恢复旧项；
8. 回滚后与原状态一致返回 `rolledBack`；
9. 回滚也失败返回 `rollbackFailed`。

**步骤 3：运行测试并确认 RED**

预期：`execute_skill_edit(...)` 及服务接口缺失。

**步骤 4：实现被动编辑规则**

公开入口：

```cpp
auto execute_skill_edit(ISkillGateway& gateway, const SkillEditRequest& request) -> SkillEditResult;
```

实现顺序：

1. `is_valid(request.target)`；
2. 读取原始状态；
3. 校验被动上限、重复项、旧项存在和新 ID 非空；
4. 调用网关；
5. 重新读取实际状态，而不是根据调用返回值猜测结果；
6. 替换失败时清除新项并恢复旧项；
7. 再次读取并判断回滚是否完整。

比较被动状态时按 ID 集合比较，不依赖游戏可能改变的列表顺序；返回给 UI 的状态保留游戏重读顺序。

**步骤 5：运行测试并确认 GREEN**

预期：所有被动编辑和回滚测试通过。

**步骤 6：提交**

```powershell
git add mods/MyPalMod/src/skill_editor_service.hpp mods/MyPalMod/tests/skill_editor_tests.cpp
git commit -m "feat: add transactional passive skill edits"
```

---

### 任务 4：测试并实现三个主动技能槽位的重写和回滚

**文件：**

- 修改：`mods/MyPalMod/src/skill_editor_service.hpp`
- 修改：`mods/MyPalMod/tests/skill_editor_tests.cpp`

**步骤 1：写主动技能失败测试**

至少覆盖：

1. `activeSlot >= 3` 被拒绝；
2. 新增只能发生在 `activeSlot == activeSkills.size()` 的第一个空尾槽；
3. 替换/清除只能作用于 `activeSlot < activeSkills.size()` 的已有槽；
4. 当前已装备 3 个技能时不能向第 4 个位置新增；
5. 不能装备当前列表中已存在的技能；
6. 空尾槽新增后保持既有顺序；
7. 替换中间槽后保持其他槽位顺序；
8. 清除中间槽后后续技能前移，结果仍最多 3 个；
9. `rewrite_active()` 返回成功但重读状态不一致时，重写完整原列表；
10. 原列表恢复成功返回 `rolledBack`；
11. 原列表恢复失败返回 `rollbackFailed`。

关键测试：

```cpp
gateway.state.activeSkills = {{1, "FireBall"}, {2, "WaterGun"}, {3, "WindCutter"}};
const SkillEditRequest request{
    .target = 0x1234,
    .kind = SkillKind::active,
    .operation = SkillEditOperation::replace,
    .activeSlot = 1,
    .newActiveSkill = ActiveSkill{4, "IceMissile"},
};

const auto result = execute_skill_edit(gateway, request);
CHECK(result.status == SkillEditStatus::succeeded);
CHECK(result.state.activeSkills ==
      std::vector<ActiveSkill>{{1, "FireBall"}, {4, "IceMissile"}, {3, "WindCutter"}});
```

**步骤 2：运行测试并确认 RED**

预期：主动技能分支未实现或断言失败。

**步骤 3：实现“完整序列重写”**

服务层先由原始 `activeSkills` 构造目标序列，然后只调用一次：

```cpp
gateway.rewrite_active(request.target, desiredSkills);
```

不得用单个 `RemoveEquipWaza()` 完成中间槽替换，因为删除会改变顺序。适配层后续将把 `rewrite_active()` 映射为：

1. `ClearEquipWaza()`；
2. 按目标序列依次 `AddEquipWaza()`；
3. 重读 `GetEquipWaza()`；
4. 不一致时再次清空并按原序列恢复。

服务层负责决定何时回滚和确认结果；UE 适配层只负责准确执行一次序列重写。

**步骤 4：运行测试并确认 GREEN**

预期：主动与被动测试全部通过。

**步骤 5：提交**

```powershell
git add mods/MyPalMod/src/skill_editor_service.hpp mods/MyPalMod/tests/skill_editor_tests.cpp
git commit -m "feat: add ordered active skill slot edits"
```

---

### 任务 5：测试并实现线程安全 FIFO 请求队列

**文件：**

- 修改：`mods/MyPalMod/src/skill_editor_service.hpp`
- 修改：`mods/MyPalMod/tests/skill_editor_tests.cpp`

**步骤 1：写失败测试**

测试按 A、B、C 顺序入队后，三次 `try_pop()` 必须严格得到 A、B、C；空队列返回 `std::nullopt`；同时验证
`size()` 和 `contains_target()`。

**步骤 2：实现队列**

```cpp
class SkillEditQueue
{
public:
    auto push(SkillEditRequest request) -> void;
    auto try_pop() -> std::optional<SkillEditRequest>;
    auto size() const -> std::size_t;
    auto contains_target(SkillTarget target) const -> bool;

private:
    mutable std::mutex mutex_;
    std::deque<SkillEditRequest> requests_;
};
```

不要提供 `drain()`：`on_update()` 每帧最多处理一个技能修改请求，避免单帧连续执行多次 Unreal 写操作。

**步骤 3：运行测试并确认 GREEN**

预期：FIFO、空队列和目标查询测试全部通过。

**步骤 4：提交**

```powershell
git add mods/MyPalMod/src/skill_editor_service.hpp mods/MyPalMod/tests/skill_editor_tests.cpp
git commit -m "feat: add FIFO skill edit request queue"
```

---

### 任务 6：实现 Palworld 技能状态与写操作适配层

**文件：**

- 新建：`mods/MyPalMod/src/pal_skills.hpp`
- 新建：`mods/MyPalMod/src/pal_skills.cpp`
- 修改：`mods/MyPalMod/src/pal_game.hpp`
- 修改：`mods/MyPalMod/CMakeLists.txt`

**步骤 1：先让 DLL 构建因缺少适配实现而失败**

在 `pal_skills.hpp` 声明：

```cpp
namespace pal_skills
{
class PalSkillGateway final : public skill_editor::ISkillGateway
{
public:
    auto is_valid(skill_editor::SkillTarget target) const -> bool override;
    auto read_state(skill_editor::SkillTarget target) -> skill_editor::SkillState override;
    auto add_passive(skill_editor::SkillTarget target, std::string_view id) -> bool override;
    auto remove_passive(skill_editor::SkillTarget target, std::string_view id) -> bool override;
    auto rewrite_active(
        skill_editor::SkillTarget target,
        std::span<const skill_editor::ActiveSkill> skills) -> bool override;
};
}
```

把 `pal_skills.cpp` 加入 `add_library(MyPalMod SHARED ...)`，暂不实现方法。构建 `MyPalMod`，确认链接失败来自这些方法。

**步骤 2：定义真实 Unreal 参数类型**

`pal_skills.cpp` 使用：

```cpp
#include <Unreal/Core/Containers/Array.hpp>
#include <Unreal/FText.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

enum class EPalWazaID : std::uint16_t
{
};
```

`GetPassiveSkillList` 和 `GetEquipWaza` 的参数必须使用：

```cpp
struct GetPassiveSkillListParams
{
    RC::Unreal::TArray<RC::Unreal::FName> ReturnValue;
};

struct GetEquipWazaParams
{
    RC::Unreal::TArray<EPalWazaID> ReturnValue;
};
```

不得保留或复制当前的 `{FName* Data; int32_t Num; int32_t Max;}` 写法。

**步骤 3：实现目标有效性和状态读取**

- 将 `SkillTarget` 转回 `UObject*`；
- 复用 `pal_game::is_valid()` 的对象有效性规则；
- 查找并缓存：
  - `/Script/Pal.PalIndividualCharacterParameter:GetPassiveSkillList`
  - `/Script/Pal.PalIndividualCharacterParameter:GetEquipWaza`
- `ProcessEvent()` 后遍历 `TArray::Num()` 和 `operator[]`；
- 主动技能的原始 ID 通过后续目录中的 `value -> id` 映射获取；映射暂不可用时使用十进制值作为稳定回退 ID；
- 主动列表只接受前 3 个，超过 3 个时记录 Warning，但不写回。

**步骤 4：实现被动技能公开函数调用**

调用：

- `AddPassiveSkill(FName AddSkill, FName OverrideSkill)`，其中 `OverrideSkill` 为空 `FName`；
- `RemovePassiveSkill(FName SkillId)`。

返回值表示函数与目标是否有效、调用是否已发出；最终成败仍由服务层的重读结果判断。

**步骤 5：实现主动技能完整序列重写**

- 调用 `ClearEquipWaza()`；
- 对目标序列按顺序调用 `AddEquipWaza(EPalWazaID)`；
- 任一函数缺失或目标失效时返回 `false`；
- 不调用 `RemoveEquipWaza()` 实现替换；
- 不读取或修改 `MasteredWaza`。

**步骤 6：删除旧的被动裸数组辅助函数**

从 `pal_game.hpp` 删除或迁移以下旧接口，避免出现两套写逻辑：

- `add_passive(...)`
- `remove_passive(...)`
- `read_pal_passives(...)`

`scan_pals()`、物品和背包逻辑保持不变。

**步骤 7：构建验证**

```powershell
cmake --build --preset ninja-msvc-x64 --target MyPalModSkillTests
ctest --test-dir build --output-on-failure
cmake --build --preset ninja-msvc-x64
```

预期：纯测试和 DLL 都成功；编译器不报告 `TArray` 析构、对齐或不完整类型问题。

**步骤 8：提交**

```powershell
git add mods/MyPalMod/CMakeLists.txt mods/MyPalMod/src/pal_skills.hpp mods/MyPalMod/src/pal_skills.cpp mods/MyPalMod/src/pal_game.hpp
git commit -m "feat: add Palworld skill runtime gateway"
```

---

### 任务 7：加载全部可选技能及中文本地化名称

**文件：**

- 修改：`mods/MyPalMod/src/skill_catalog.hpp`
- 修改：`mods/MyPalMod/src/pal_skills.hpp`
- 修改：`mods/MyPalMod/src/pal_skills.cpp`
- 修改：`mods/MyPalMod/tests/skill_editor_tests.cpp`

**步骤 1：为目录快照和失败状态写测试**

扩展纯模型：

```cpp
struct SkillCatalogSnapshot
{
    std::vector<SkillOption> passiveSkills;
    std::vector<SkillOption> activeSkills;
    std::string error;
    bool ready{};
};
```

测试“加载失败时保留上一次成功缓存”的纯辅助函数；没有成功缓存时 `ready == false`，UI 后续必须禁用下拉框。

**步骤 2：实现被动技能主加载路径**

在游戏线程中：

1. `FindFirstOf("PalPassiveSkillManager")` 获取管理器实例；
2. 调用 `GetPalAssignablePassiveIDs(TArray<FName>& List)`；
3. 对每个 ID 调用 `UPalUIUtility::GetPassiveSkillName(WorldContext, Id, FText&)`；
4. 用 `FText::ToString()` 转为 UTF-8 `std::string`；
5. 本地化为空时只保留原始 ID；
6. 去重后写入目录快照。

`GetPalAssignablePassiveIDs` 的出参同样必须是：

```cpp
struct GetPalAssignablePassiveIdsParams
{
    RC::Unreal::TArray<RC::Unreal::FName> List;
};
```

**步骤 3：实现被动技能回退路径**

主调用失败时依次尝试：

1. 从 `PalAssignableSkillMap` 反射属性提取键；
2. 若同时能读到 `PassiveSkillDataTable`，只用其行名补全上述可分配键的显示数据，不能把全部行直接加入目录；
3. 若仍失败，保留进程内上一次成功目录；
4. 若没有缓存，设置明确错误并禁用被动技能编辑。

这样即使回退，也不会把不可分配的被动技能暴露到下拉框。

**步骤 4：实现主动技能目录**

1. 查找 `/Script/Pal.EPalWazaID` 的 `UEnum`；
2. 遍历 `GetEnumNames()`；
3. 只接受 `0..65535`；
4. 去掉 `EPalWazaID::` 前缀；
5. 排除空 ID、`None`、`MAX`、`EPalWazaID_MAX` 等哨兵；
6. 对每个值调用 `UPalUIUtility::GetWazaName(WorldContext, Value, FText&)`；
7. 构造带 `activeValue` 的 `SkillOption`；
8. 按枚举数值去重，避免同值别名绕过“已装备技能隐藏”规则；
9. 同时维护 `value -> raw id` 映射，供状态读取使用。

**步骤 5：安全调用本地化函数**

- 查找 `Default__PalUIUtility` 默认对象；
- WorldContext 使用当前有效目标；没有目标时使用当前有效的游戏实例上下文；
- 参数结构使用真实 `FName`、`FText` 和 `EPalWazaID`；
- 使用 `WideCharToMultiByte(CP_UTF8, ...)` 辅助函数把 UE4SS `StringType`/宽字符串转换为 UTF-8，
  不得用 `std::string(wide.begin(), wide.end())` 截断中文；
- 任何本地化失败只影响显示名，不得丢掉合法的原始技能 ID；
- 所有目录加载都在 `on_update()` 触发，不能从 ImGui 回调调用。

**步骤 6：验证**

运行 CTest 和完整 DLL 构建。静态检查目录代码中不存在：

```powershell
rg -n "FName\\* Data|int32_t Num.*int32_t Max" mods/MyPalMod/src
```

预期：无匹配。

**步骤 7：提交**

```powershell
git add mods/MyPalMod/src/skill_catalog.hpp mods/MyPalMod/src/pal_skills.hpp mods/MyPalMod/src/pal_skills.cpp mods/MyPalMod/tests/skill_editor_tests.cpp
git commit -m "feat: load localized passive and active skill catalogs"
```

---

### 任务 8：接入目标解析、游戏线程队列和 UI 快照

**文件：**

- 修改：`mods/MyPalMod/src/dllmain.cpp`

**步骤 1：增加线程边界清晰的状态**

在 `MyPalMod` 中增加：

```cpp
enum class SkillTargetSource
{
    none,
    viewed,
    selected,
};

struct SkillEditorSnapshot
{
    skill_editor::SkillTarget target{};
    SkillTargetSource source{SkillTargetSource::none};
    std::string palName;
    skill_editor::SkillState state;
    skill_editor::SkillCatalogSnapshot catalog;
    std::string lastResult;
    bool pending{};
};

pal_skills::PalSkillGateway skillGateway_;
skill_editor::SkillEditQueue skillQueue_;
std::mutex skillSnapshotMutex_;
SkillEditorSnapshot skillSnapshot_;
std::atomic<bool> wantRefreshSkillCatalog_{true};
```

UI 自己的搜索框、当前下拉选择 ID 保持为 GUI 线程成员，不放进游戏状态快照。

**步骤 2：实现目标解析优先级**

在游戏线程辅助函数中：

1. 读取 `lastViewedPal_`；
2. 若 `skillGateway_.is_valid(viewed)`，返回来源 `viewed`；
3. 否则在 `inv_mutex_` 保护下复制手动选择的 `UObject*`；
4. 若手动目标有效，返回来源 `selected`；
5. 否则返回空目标。

不要让 UI 直接解引用 `UObject*`。

**步骤 3：每帧刷新只读快照**

`on_update()` 中：

- 解析当前目标；
- 目标变化或显式刷新时读取技能状态；
- 首次进入或点击“刷新技能列表”时加载目录；
- 目录和状态先在游戏线程局部变量中构造完整，再用一次加锁替换 `skillSnapshot_`；
- 当前查看目标失效时自动退回手动选择；
- 没有目标时快照清空状态，但保留已加载目录缓存。

**步骤 4：按 FIFO 每帧执行一个请求**

`on_update()` 中：

```cpp
if (auto request = skillQueue_.try_pop())
{
    const auto result = skill_editor::execute_skill_edit(skillGateway_, *request);
    // 将 result.state、message 和 pending 写入快照
}
```

要求：

- 不把请求目标替换成“当前目标”；必须使用点击时捕获的 `request.target`；
- 执行前由服务再次校验目标；
- 结果状态来自实际重读；
- 队列仍有请求时保持 `pending == true`；
- 完成后再次刷新当前目标快照，防止 UI 显示旧状态。

**步骤 5：移除旧的单槽被动请求字段**

删除：

- `passive_buf_`
- `passive_pal_`
- `passive_skill_`
- `passive_add_`
- `passive_requested_`
- `passive_read_requested_`

以及 `on_update()` 中对应的旧分支。

**步骤 6：构建验证**

运行纯测试和 DLL 构建。重点检查锁顺序：不得在持有 `inv_mutex_` 时再等待 `skillSnapshotMutex_`，反之亦然；
跨缓存数据先复制再释放锁。

**步骤 7：提交**

```powershell
git add mods/MyPalMod/src/dllmain.cpp
git commit -m "feat: route skill edits through game-thread queue"
```

---

### 任务 9：实现主动/被动技能下拉编辑 UI

**文件：**

- 修改：`mods/MyPalMod/src/dllmain.cpp`

**步骤 1：替换当前手输 ID 区域**

保留 “Scan Pals” 列表作为回退选择，但将两套被动输入框合并为一个 `render_skill_editor()`。

顶部显示：

- `目标：<帕鲁名>`；
- 来源：`当前查看` 或 `手动选择`；
- 无有效目标时的操作提示；
- `刷新技能列表` 按钮；
- 最近一次操作结果；
- 目录加载错误。

**步骤 2：实现被动技能区域**

- 当前技能最多显示 4 行；
- 每行显示本地化标签，未知 ID 显示原始 ID；
- 每行提供“替换”和“删除”；
- 未满 4 个时显示“新增”；
- 替换/新增先在可搜索下拉框中选择，不立即修改；
- 下拉候选隐藏当前已拥有的被动；
- 点击确认时构造完整 `SkillEditRequest` 并 `skillQueue_.push(...)`。

**步骤 3：实现主动技能区域**

- 固定显示槽位 1、2、3；
- 已占用槽位提供“替换”和“清空”；
- 空槽位提供“选择/装备”；
- 下拉候选隐藏当前已装备技能；
- 替换请求携带槽位和 `ActiveSkill{value, id}`；
- 清空使用 `operation = remove`；
- 不显示或编辑 `MasteredWaza`。

**步骤 4：实现搜索下拉组件**

组件输入：

```cpp
auto render_skill_picker(
    const char* id,
    std::span<const skill_editor::SkillOption> options,
    const std::unordered_set<std::string>& excludedIds,
    std::string& search,
    std::optional<skill_editor::SkillOption>& selected) -> bool;
```

使用 `skill_label()` 显示 `中文名 [RawId]`，搜索调用 `filter_skills()`。选择只更新 GUI 本地状态；
只有点击“新增/确认替换/装备”才投递请求。

**步骤 5：处理 pending 和过期快照**

- `snapshot.pending` 或队列非空时禁用所有技能修改按钮；
- 刷新目录按钮可在 pending 时禁用；
- UI 每帧复制一次 `skillSnapshot_` 后立即释放锁；
- 点击按钮时使用这份快照中的目标地址；
- 操作结束或目标切换后清理尚未确认的下拉选择，防止把旧选择应用到新帕鲁。

**步骤 6：构建并人工检查 UI 代码**

确认：

- 不再出现 `InputText("Passive SkillId"...`；
- 所有 ImGui ID 带槽位或技能 ID 后缀，避免控件 ID 冲突；
- 没有在渲染函数中调用 `ProcessEvent()`、`StaticFindObject()` 或对象有效性检查。

**步骤 7：提交**

```powershell
git add mods/MyPalMod/src/dllmain.cpp
git commit -m "feat: add searchable active and passive skill editor UI"
```

---

### 任务 10：更新版本、文档并完成自动化验证

**文件：**

- 修改：`mods/MyPalMod/src/dllmain.cpp`
- 修改：`README.md`
- 必要时修改：`docs/superpowers/specs/2026-07-23-pal-skill-editor-design.md`

**步骤 1：统一版本为 1.4.0**

同时更新：

- `ModVersion = STR("1.4.0")`
- 加载日志中的版本；
- ImGui 窗口标题；
- README 功能/版本说明。

不得留下 `v1.3`、`1.3.1` 或 `1.3.2` 的当前版本标识。

**步骤 2：更新 README**

写明：

- 当前查看帕鲁优先、手动扫描选择回退；
- 被动技能最多 4 个，可新增/替换/删除；
- 主动技能只编辑三个 `EquipWaza` 槽位；
- 下拉框支持中文名和原始 ID 搜索；
- 不修改伙伴技能和 `MasteredWaza`；
- 支持单机与房主/本地主机，普通客户端不支持；
- 失败时会重读并尝试回滚；
- 构建、部署和游戏内验证方法。

**步骤 3：运行完整自动化验证**

```powershell
cmake --preset ninja-msvc-x64
cmake --build --preset ninja-msvc-x64 --target MyPalModSkillTests
ctest --test-dir build --output-on-failure
cmake --build --preset ninja-msvc-x64
rg -n "FName\\* Data|Passive SkillId|v1\\.3|1\\.3\\.[0-9]" mods/MyPalMod/src README.md
git diff --check
git status --short
```

预期：

- 测试 100% 通过；
- DLL 构建成功；
- `rg` 无匹配；
- `git diff --check` 无输出；
- `git status` 只包含本功能文件及用户原有未提交文件。

**步骤 4：提交**

```powershell
git add mods/MyPalMod/src/dllmain.cpp README.md
git commit -m "docs: release skill editor v1.4.0"
```

若设计文档确因实现细节发生变化，单独精确加入该文件；否则不要为了制造改动而修改它。

---

### 任务 11：部署并完成 Palworld 游戏内端到端验收

**文件：**

- 不新增代码；记录 UE4SS 日志与人工验收结果。

**步骤 1：部署**

```powershell
cmake --build --preset ninja-msvc-x64 --target deploy
```

预期：

- `Pal/Binaries/Win64/ue4ss/Mods/MyPalMod/dlls/main.dll` 更新；
- `enabled.txt` 存在；
- UE4SS 控制台显示 `MyPalMod loaded (v1.4.0)`。

**步骤 2：验收目标解析**

1. 打开 Palbox/队伍详情，确认顶部显示“当前查看”目标；
2. 关闭详情并让该对象失效或切换场景，确认能退回手动选择；
3. “Scan Pals” 后选中另一只帕鲁，确认来源显示“手动选择”；
4. 在目标切换前排队操作，确认请求不会错误应用到新目标。

**步骤 3：验收被动技能**

1. 下拉框能列出可分配被动，显示中文名与原始 ID；
2. 中文名和 Raw ID 搜索都能找到相同技能；
3. 已拥有技能不出现在新增候选；
4. 新增、替换、删除分别成功；
5. 第 4 个被动存在时新增按钮禁用；
6. 操作后 UI 显示游戏重读状态；
7. 制造一次无效/失败操作，确认错误可见且原技能被恢复。

**步骤 4：验收主动技能**

1. 固定显示 3 个 `EquipWaza` 槽位；
2. 对空槽装备、对已有槽替换、清空中间槽；
3. 确认替换前后其他技能顺序不变；
4. 已装备技能不出现在候选；
5. 确认 `MasteredWaza` 没有被修改。

**步骤 5：验收持久化和环境边界**

1. 保存并返回标题界面；
2. 重新载入存档；
3. 确认主动/被动技能修改仍然存在；
4. 至少在单机完成一次；若有本地主机环境，再完成一次房主验证；
5. README 明确标注普通客户端不支持，不把客户端未生效判定为本版本回归。

**步骤 6：最终复核**

```powershell
git log --oneline -12
git status --short --branch
```

确认每项提交只包含计划内文件，用户原有未提交内容仍保持原状。若游戏内发现问题，先使用
`superpowers:systematic-debugging` 定位根因，再补失败测试和修复；不得绕过重读、回滚或线程边界。

---

## 规格覆盖自检

- [ ] 只编辑主动技能和被动技能，不含伙伴技能。
- [ ] 主动技能只编辑三个 `EquipWaza` 槽，不碰 `MasteredWaza`。
- [ ] 被动目录只含 Pal-assignable 技能。
- [ ] 主动/被动目录都支持中文名与 Raw ID 搜索。
- [ ] 当前查看目标优先，手动选择作为回退。
- [ ] 请求捕获点击时目标，并由游戏线程 FIFO 执行。
- [ ] 所有修改后重读真实状态。
- [ ] 被动替换和主动序列重写都有回滚。
- [ ] 不直接写游戏内部数组、DataTable 或 SaveParameter。
- [ ] 所有 Unreal 数组出参使用真实 `TArray<T>`。
- [ ] 有纯 C++ CTest 覆盖搜索、过滤、4 被动上限、3 主动槽、回滚、过期目标和 FIFO。
- [ ] 有游戏内保存/重载持久化验收。
- [ ] 版本统一更新到 1.4.0。
