# 被动技能分类选择器 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在被动技能新增与替换流程中提供“类别 + 技能”两级下拉框，同时完整保留中文名和 Raw ID 搜索，并以有界的游戏线程增量任务读取 `Rank` 与 `AddWorldTreePal`，避免引入常驻扫描或明显丢帧。

**Architecture:** `skill_catalog.hpp` 承担纯 C++ 分类模型、过滤、选择器状态和增量任务状态机；`PalSkillGateway` 每个 EngineTick 最多读取 8 个技能且受 500 微秒软预算约束；`dllmain.cpp` 只在技能目录刷新后驱动任务、通过原子进度向 GUI 报告，并在完成时一次性发布分类后的目录快照。GUI 继续使用现有 UE4SS ImGui 页签和既有技能写入服务，不改变目标选择、编辑请求或主动技能流程。

**Tech Stack:** C++23、UE4SS 反射 API、ImGui、CMake/Ninja、现有无 Unreal 依赖的 `PalworldEditorTests`。

## Global Constraints

- 分类名称固定为：`全部`、`普通`、`稀有`、`极品`、`传说`、`负面`。
- 分类规则固定为：
  - `AddWorldTreePal == true`：传说；
  - 否则 `Rank < 0`：负面；
  - 否则 `Rank >= 4`：极品；
  - 否则 `Rank == 3`：稀有；
  - 否则：普通。
- 分类颜色固定为：
  - 普通：白色；
  - 稀有：黄色；
  - 极品：蓝色；
  - 传说：紫色；
  - 负面：红色。
- “全部”不是技能元数据分类，而是 GUI 过滤条件中的 `std::nullopt`。
- 搜索顺序固定为：类别过滤 → 排除已装备技能 → 中文名/Raw ID 模糊搜索。
- 切换类别时清空当前待新增/替换的技能选择，但保留搜索文本。
- 首次分类完成前只有“全部”可选；若刷新时已有可用分类，结构性失败要保留旧分类并显示警告。
- 单个技能的 `GetSkillData` 返回 `false` 只把该技能标为“未知”，不得终止整个任务；未知技能仅出现在“全部”。
- 不读取 `PalPassiveSkillManager` 私有 `TMap`，不硬编码 `FPalPassiveSkillDatabaseRow` 的完整内存布局。
- 不在 GUI 回调、空闲目标解析、技能编辑请求或 LoadMap 回调中读取技能元数据。
- 不缓存跨帧 `UObject*`、`UFunction*`、`FProperty*`、参数缓冲区或结构体地址。
- 每个 EngineTick 最多处理 8 个 ID，并在每次 `ProcessEvent` 后检查 500 微秒软预算。
- 成功读取的 `{Raw ID -> Rank/AddWorldTreePal/Category}` 纯数据缓存保留到 mod 卸载；手动刷新只重试新 ID 和先前失败 ID。
- LoadMap 前取消未完成任务和撤销分类写权限，但保留纯标准库成功缓存。
- 分类运行期间不得逐技能复制并发布整个 GUI 技能目录；进度通过原子计数读取，目录仅在任务开始和完成/失败时发布。
- 不改变主动技能选择器、被动技能写入方式、四词条预设、当前帕鲁锁定规则和资源共享实现。

---

## Task 1: 建立纯 C++ 被动技能分类、过滤和选择器状态

**Files:**

- Modify: `mods/PalworldEditor/inc/skills/skill_catalog.hpp`
- Modify: `mods/PalworldEditor/tests/skill_editor_tests.cpp`

- [ ] **Step 1: 先添加分类规则和过滤顺序的失败测试**

在 `skill_editor_tests.cpp` 增加以下测试，并在 `main()` 中调用：

```cpp
void test_passive_skill_categories_follow_runtime_metadata()
{
    using PalworldEditor::Skills::PassiveSkillCategory;
    using PalworldEditor::Skills::classify_passive_skill;

    assert(classify_passive_skill(0, false) == PassiveSkillCategory::normal);
    assert(classify_passive_skill(2, false) == PassiveSkillCategory::normal);
    assert(classify_passive_skill(3, false) == PassiveSkillCategory::rare);
    assert(classify_passive_skill(4, false) == PassiveSkillCategory::premium);
    assert(classify_passive_skill(9, false) == PassiveSkillCategory::premium);
    assert(classify_passive_skill(-1, false) == PassiveSkillCategory::negative);
    assert(classify_passive_skill(-1, true) == PassiveSkillCategory::legendary);
    assert(classify_passive_skill(2, true) == PassiveSkillCategory::legendary);
}

void test_passive_filter_combines_category_exclusion_and_search()
{
    using namespace PalworldEditor::Skills;

    const std::vector<SkillOption> skills{
        {.id = "Passive_Normal",
         .localizedName = "普通技能",
         .passiveMetadata = PassiveSkillMetadata{.rank = 1, .addWorldTreePal = false,
                                                  .category = PassiveSkillCategory::normal}},
        {.id = "Passive_Rare",
         .localizedName = "稀有采矿",
         .passiveMetadata = PassiveSkillMetadata{.rank = 3, .addWorldTreePal = false,
                                                  .category = PassiveSkillCategory::rare}},
        {.id = "Passive_Legend",
         .localizedName = "传说采矿",
         .passiveMetadata = PassiveSkillMetadata{.rank = 1, .addWorldTreePal = true,
                                                  .category = PassiveSkillCategory::legendary}},
        {.id = "Passive_Unknown", .localizedName = "未知采矿"},
    };
    const std::unordered_set<std::string> equipped{"Passive_Legend"};

    const auto rare =
        filter_passive_skills(skills, PassiveSkillCategory::rare, "采矿", equipped);
    assert(rare.size() == 1);
    assert(rare.front().id == "Passive_Rare");

    const auto all = filter_passive_skills(skills, std::nullopt, "Passive_", equipped);
    assert(all.size() == 3);
    assert(std::ranges::any_of(all, [](const SkillOption& option) {
        return option.id == "Passive_Unknown";
    }));
}
```

测试必须同时证明：

- `AddWorldTreePal` 优先于负 Rank；
- 未知元数据不会进入具体类别；
- “全部”仍包含未知技能；
- 排除已装备技能与搜索条件仍然生效；
- Raw ID 搜索仍然生效。

- [ ] **Step 2: 运行测试并确认编译失败**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorTests
```

Expected: 编译失败，提示 `PassiveSkillCategory`、`PassiveSkillMetadata`、`classify_passive_skill` 或 `filter_passive_skills` 尚不存在。

- [ ] **Step 3: 在目录模型中加入最小分类实现**

在 `skill_catalog.hpp` 中加入：

```cpp
enum class PassiveSkillCategory
{
    normal,
    rare,
    premium,
    legendary,
    negative,
};

struct PassiveSkillMetadata
{
    std::int32_t rank{};
    bool addWorldTreePal{};
    PassiveSkillCategory category{PassiveSkillCategory::normal};

    auto operator<=>(const PassiveSkillMetadata&) const = default;
};

[[nodiscard]] constexpr auto classify_passive_skill(const std::int32_t rank,
                                                     const bool addWorldTreePal)
    -> PassiveSkillCategory
{
    if (addWorldTreePal)
    {
        return PassiveSkillCategory::legendary;
    }
    if (rank < 0)
    {
        return PassiveSkillCategory::negative;
    }
    if (rank >= 4)
    {
        return PassiveSkillCategory::premium;
    }
    if (rank == 3)
    {
        return PassiveSkillCategory::rare;
    }
    return PassiveSkillCategory::normal;
}
```

向 `SkillOption` 增加：

```cpp
std::optional<PassiveSkillMetadata> passiveMetadata;
```

新增 `filter_passive_skills`，明确按以下顺序判断，避免后续 GUI 各自拼装不同过滤逻辑：

```cpp
[[nodiscard]] inline auto filter_passive_skills(
    const std::span<const SkillOption> options,
    const std::optional<PassiveSkillCategory> category,
    const std::string_view query,
    const std::unordered_set<std::string>& excludedIds) -> std::vector<SkillOption>
{
    std::vector<SkillOption> filtered;
    for (const auto& option : options)
    {
        if (category.has_value()
            && (!option.passiveMetadata.has_value()
                || option.passiveMetadata->category != *category))
        {
            continue;
        }
        if (excludedIds.contains(option.id) || !matches_skill(option, query))
        {
            continue;
        }
        filtered.push_back(option);
    }
    return filtered;
}
```

保留现有通用 `filter_skills` 供主动技能选择器使用。

- [ ] **Step 4: 添加并测试类别切换状态**

测试：

```cpp
void test_passive_picker_category_change_clears_only_selection()
{
    using namespace PalworldEditor::Skills;

    PassiveSkillPickerState state{
        .category = std::nullopt,
        .selected = SkillOption{.id = "Passive_Rare", .localizedName = "稀有采矿"},
    };

    assert(state.set_category(PassiveSkillCategory::rare));
    assert(state.category == PassiveSkillCategory::rare);
    assert(!state.selected.has_value());
    assert(!state.set_category(PassiveSkillCategory::rare));
}
```

在 `skill_catalog.hpp` 实现：

```cpp
struct PassiveSkillPickerState
{
    std::optional<PassiveSkillCategory> category;
    std::optional<SkillOption> selected;

    [[nodiscard]] auto set_category(
        const std::optional<PassiveSkillCategory> nextCategory) -> bool
    {
        if (category == nextCategory)
        {
            return false;
        }
        category = nextCategory;
        selected.reset();
        return true;
    }

    void clear_selection()
    {
        selected.reset();
    }

    void reset()
    {
        category.reset();
        selected.reset();
    }
};
```

搜索缓冲区仍由 `dllmain.cpp` 单独持有，因此类别切换不会清空搜索文本。

- [ ] **Step 5: 运行纯 C++ 测试**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorTests
ctest --test-dir build --output-on-failure
```

Expected: 新增测试和现有测试全部通过。

- [ ] **Step 6: 提交纯模型**

```powershell
git add mods/PalworldEditor/inc/skills/skill_catalog.hpp mods/PalworldEditor/tests/skill_editor_tests.cpp
git commit -m "feat: add passive skill category model"
```

---

## Task 2: 建立可测试的增量分类任务和成功缓存合并逻辑

**Files:**

- Modify: `mods/PalworldEditor/inc/skills/skill_catalog.hpp`
- Modify: `mods/PalworldEditor/tests/skill_editor_tests.cpp`

- [ ] **Step 1: 添加缓存复用、失败重试和批次推进的失败测试**

新增：

```cpp
void test_passive_classification_job_reuses_success_cache_and_retries_unknowns()
{
    using namespace PalworldEditor::Skills;

    const std::vector<SkillOption> skills{
        {.id = "Cached", .localizedName = "已缓存"},
        {.id = "Retry", .localizedName = "待重试"},
        {.id = "New", .localizedName = "新增"},
    };
    std::unordered_map<std::string, PassiveSkillMetadata> cache{
        {"Cached",
         PassiveSkillMetadata{.rank = 3,
                              .addWorldTreePal = false,
                              .category = PassiveSkillCategory::rare}},
    };

    PassiveSkillClassificationJob job;
    job.start(skills, cache);
    assert(job.status().total == 3);
    assert(job.status().completed == 1);
    assert(job.next_batch(8) == std::vector<std::string>({"Retry", "New"}));

    const std::vector<PassiveSkillMetadataReadResult> firstBatch{
        {.id = "Retry", .metadata = std::nullopt},
        {.id = "New",
         .metadata = PassiveSkillMetadata{.rank = 4,
                                          .addWorldTreePal = false,
                                          .category = PassiveSkillCategory::premium}},
    };
    assert(job.complete_batch(firstBatch, cache));
    assert(job.status().ready);
    assert(cache.contains("New"));
    assert(!cache.contains("Retry"));

    job.start(skills, cache);
    assert(job.next_batch(8) == std::vector<std::string>({"Retry"}));
}
```

再添加：

```cpp
void test_passive_classification_job_honors_batch_limit_and_reports_structural_failure()
{
    using namespace PalworldEditor::Skills;

    std::vector<SkillOption> skills;
    for (int index = 0; index < 10; ++index)
    {
        skills.push_back(
            {.id = "Passive_" + std::to_string(index), .localizedName = "技能"});
    }
    std::unordered_map<std::string, PassiveSkillMetadata> cache;

    PassiveSkillClassificationJob job;
    job.start(skills, cache);
    assert(job.next_batch(8).size() == 8);

    job.fail("missing Rank property");
    assert(!job.active());
    assert(!job.status().ready);
    assert(job.status().error == "missing Rank property");
}
```

- [ ] **Step 2: 运行测试并确认编译失败**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorTests
```

Expected: 编译失败，提示增量任务及读取结果类型不存在。

- [ ] **Step 3: 添加读取结果、状态和增量任务**

在 `skill_catalog.hpp` 中加入：

```cpp
struct PassiveSkillMetadataReadResult
{
    std::string id;
    std::optional<PassiveSkillMetadata> metadata;
};

struct PassiveSkillMetadataBatchResult
{
    std::vector<PassiveSkillMetadataReadResult> entries;
    std::string error;
    std::chrono::microseconds elapsed{};
};

struct PassiveSkillClassificationStatus
{
    std::size_t completed{};
    std::size_t total{};
    std::string error;
    bool ready{};
};
```

`PassiveSkillClassificationJob` 仅持有 Raw ID、计数和错误字符串，不持有 Unreal 句柄：

```cpp
class PassiveSkillClassificationJob
{
public:
    void start(
        const std::span<const SkillOption> options,
        const std::unordered_map<std::string, PassiveSkillMetadata>& successCache);
    void cancel();
    void fail(std::string error);

    [[nodiscard]] auto next_batch(std::size_t limit) const
        -> std::vector<std::string>;
    [[nodiscard]] auto complete_batch(
        std::span<const PassiveSkillMetadataReadResult> entries,
        std::unordered_map<std::string, PassiveSkillMetadata>& successCache) -> bool;
    [[nodiscard]] auto active() const -> bool;
    [[nodiscard]] auto status() const -> PassiveSkillClassificationStatus;

private:
    std::deque<std::string> pending_;
    std::size_t completed_{};
    std::size_t total_{};
    std::string error_;
    bool active_{};
};
```

实现约束：

- `start()` 清空上一任务，按目录顺序加入未命中成功缓存的 ID；
- 命中缓存的 ID 直接计入 `completed_`；
- `next_batch(limit)` 只复制队首最多 `limit` 个 ID，不提前弹出；
- `complete_batch()` 要求结果 ID 与队首顺序一致；顺序不一致时调用 `fail("passive classification batch order mismatch")` 并返回 `false`；
- `metadata == std::nullopt` 仍弹出并增加完成数，但不进入成功缓存；
- 队列清空后 `active_ = false`，且无结构错误时 `ready = true`；
- `cancel()` 清空任务和状态，但不得修改外部成功缓存。

- [ ] **Step 4: 测试目录元数据合并和分类状态回退**

新增测试：

```cpp
void test_passive_metadata_merge_keeps_unknown_skills_in_catalog()
{
    using namespace PalworldEditor::Skills;

    std::vector<SkillOption> skills{
        {.id = "Known", .localizedName = "已知"},
        {.id = "Unknown", .localizedName = "未知"},
    };
    const std::unordered_map<std::string, PassiveSkillMetadata> cache{
        {"Known",
         PassiveSkillMetadata{.rank = 3,
                              .addWorldTreePal = false,
                              .category = PassiveSkillCategory::rare}},
    };

    apply_passive_metadata(skills, cache);
    assert(skills[0].passiveMetadata.has_value());
    assert(!skills[1].passiveMetadata.has_value());
}

void test_catalog_fallback_preserves_previous_passive_classification()
{
    using namespace PalworldEditor::Skills;

    SkillCatalogSnapshot previous;
    previous.passive.ready = true;
    previous.passive.skills = {{.id = "Old", .localizedName = "旧技能"}};
    previous.passiveClassification = {
        .completed = 1, .total = 1, .error = {}, .ready = true};

    SkillCatalogSnapshot failedRefresh;
    failedRefresh.passive.error = "refresh failed";
    const auto merged = with_catalog_fallback(previous, failedRefresh);

    assert(merged.passive.skills.front().id == "Old");
    assert(merged.passiveClassification.ready);
    assert(merged.passive.error == "refresh failed");
}
```

向 `SkillCatalogSnapshot` 增加：

```cpp
PassiveSkillClassificationStatus passiveClassification;
```

实现：

```cpp
inline void apply_passive_metadata(
    std::vector<SkillOption>& skills,
    const std::unordered_map<std::string, PassiveSkillMetadata>& successCache)
{
    for (auto& skill : skills)
    {
        if (const auto found = successCache.find(skill.id); found != successCache.end())
        {
            skill.passiveMetadata = found->second;
        }
        else
        {
            skill.passiveMetadata.reset();
        }
    }
}
```

调整 `with_catalog_fallback()`：

- 被动目录刷新失败时沿用旧技能数组和旧 `passiveClassification`；
- 保留本次刷新错误，便于 GUI 显示；
- 主动目录仍按原逻辑独立回退；
- 被动目录刷新成功时不伪造分类成功，分类状态由游戏线程任务重新设置。

- [ ] **Step 5: 运行测试**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorTests
ctest --test-dir build --output-on-failure
```

Expected: 所有纯 C++ 测试通过。

- [ ] **Step 6: 提交增量任务模型**

```powershell
git add mods/PalworldEditor/inc/skills/skill_catalog.hpp mods/PalworldEditor/tests/skill_editor_tests.cpp
git commit -m "feat: add incremental passive classification state"
```

---

## Task 3: 通过安全反射批量读取 Rank 与 AddWorldTreePal

**Files:**

- Modify: `mods/PalworldEditor/inc/skills/pal_skills.hpp`
- Modify: `mods/PalworldEditor/src/pal_skills.cpp`

- [ ] **Step 1: 扩展网关接口**

在 `PalSkillGateway` 增加：

```cpp
[[nodiscard]] auto load_passive_skill_metadata_batch(
    std::span<const std::string> ids,
    std::size_t maxItems,
    std::chrono::microseconds budget) const -> PassiveSkillMetadataBatchResult;
```

在头文件显式包含所需的 `<chrono>`、`<span>`，返回值使用 Task 2 已定义的纯数据类型。

- [ ] **Step 2: 实现一次调用内的结构解析**

在 `pal_skills.cpp` 增加私有辅助结构，仅在函数栈上存活：

```cpp
struct PassiveSkillMetadataReflection
{
    RC::Unreal::UObject* manager{};
    RC::Unreal::UFunction* getSkillData{};
    RC::Unreal::FNameProperty* skillName{};
    RC::Unreal::FStructProperty* outSkillData{};
    RC::Unreal::FBoolProperty* returnValue{};
    RC::Unreal::FIntProperty* rank{};
    RC::Unreal::FBoolProperty* addWorldTreePal{};
};
```

解析流程必须全部发生在 `load_passive_skill_metadata_batch()` 当前调用内：

1. `UObjectGlobals::FindFirstOf(STR("PalPassiveSkillManager"))` 获取 manager；
2. 解析 `/Script/Pal.PalPassiveSkillManager:GetSkillData`；
3. 按名称定位参数 `SkillName`、`outSkillData` 和返回值；
4. 从 `outSkillData->GetStruct()` 按名称定位 `Rank` 与 `AddWorldTreePal`；
5. 校验属性实际类型；
6. 任一结构缺失时设置单条结构性错误并返回，不进入 ID 循环。

不得保存该辅助结构到 `PalSkillGateway` 字段或静态变量。

- [ ] **Step 3: 用动态参数缓冲区逐个调用 GetSkillData**

每个 ID 的实现骨架：

```cpp
std::vector<std::byte> parameters(getSkillData->GetParmsSize());
getSkillData->InitializeStruct(parameters.data());
const auto destroyParameters = make_scope_exit([&] {
    getSkillData->DestroyStruct(parameters.data());
});

const RC::Unreal::FName skillNameValue(ensure_str(id), RC::Unreal::FNAME_Add);
skillName->CopyCompleteValue(
    skillName->ContainerPtrToValuePtr<void>(parameters.data()), &skillNameValue);
manager->ProcessEvent(getSkillData, parameters.data());

const bool found =
    returnValue->GetPropertyValue_InContainer(parameters.data());
if (!found)
{
    result.entries.push_back({.id = id, .metadata = std::nullopt});
}
else
{
    void* row = outSkillData->ContainerPtrToValuePtr<void>(parameters.data());
    const auto rankValue = *rank->ContainerPtrToValuePtr<std::int32_t>(row);
    const auto worldTreeValue =
        addWorldTreePal->GetPropertyValue_InContainer(row);
    result.entries.push_back(
        {.id = id,
         .metadata = PassiveSkillMetadata{
             .rank = rankValue,
             .addWorldTreePal = worldTreeValue,
             .category = classify_passive_skill(rankValue, worldTreeValue)}});
}
```

实现时复用项目现有的作用域清理工具或相同的 RAII 模式；若 API 实际命名是
`GetPropertyValueInContainer`，以当前 RE-UE4SS 头文件声明为准，不通过 reinterpret cast 绕开类型检查。

- [ ] **Step 4: 实施数量与时间双重限制**

循环要求：

```cpp
const auto startedAt = std::chrono::steady_clock::now();
const auto count = std::min(ids.size(), maxItems);
for (std::size_t index = 0; index < count; ++index)
{
    // 每次至少允许处理一个 ID。
    // 初始化参数、ProcessEvent、提取纯数据、销毁参数。

    if (std::chrono::steady_clock::now() - startedAt >= budget)
    {
        break;
    }
}
result.elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
    std::chrono::steady_clock::now() - startedAt);
```

时间预算是软限制：单次 `ProcessEvent` 无法被中断，但必须在每次调用后检查，不得先连续处理完整批次再检查。

- [ ] **Step 5: 编译 DLL 验证 UE4SS API 类型和参数生命周期**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditor
```

Expected: `PalworldEditor.dll` 编译链接成功；无 FProperty 类型、参数初始化或返回属性 API 错误。

- [ ] **Step 6: 提交反射网关**

```powershell
git add mods/PalworldEditor/inc/skills/pal_skills.hpp mods/PalworldEditor/src/pal_skills.cpp
git commit -m "feat: read passive skill metadata incrementally"
```

---

## Task 4: 在 EngineTick 中调度分类并实现 LoadMap/失败回退

**Files:**

- Modify: `mods/PalworldEditor/src/dllmain.cpp`
- Modify: `mods/PalworldEditor/tests/skill_editor_tests.cpp`

- [ ] **Step 1: 补充任务取消不清空成功缓存的回归测试**

新增：

```cpp
void test_passive_classification_cancel_does_not_mutate_success_cache()
{
    using namespace PalworldEditor::Skills;

    const std::vector<SkillOption> skills{
        {.id = "Known", .localizedName = "已知"},
        {.id = "Pending", .localizedName = "待处理"},
    };
    std::unordered_map<std::string, PassiveSkillMetadata> cache{
        {"Known",
         PassiveSkillMetadata{.rank = 3,
                              .addWorldTreePal = false,
                              .category = PassiveSkillCategory::rare}},
    };

    PassiveSkillClassificationJob job;
    job.start(skills, cache);
    job.cancel();

    assert(!job.active());
    assert(cache.contains("Known"));
}
```

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorTests
ctest --test-dir build --output-on-failure
```

Expected: 通过，锁定 LoadMap 所依赖的纯数据生命周期。

- [ ] **Step 2: 增加运行时字段和固定预算**

在 `PalworldEditorMod` 增加：

```cpp
static constexpr std::size_t kPassiveMetadataBatchSize = 8;
static constexpr auto kPassiveMetadataBudget = std::chrono::microseconds{500};

Skills::PassiveSkillClassificationJob passiveClassificationJob_;
std::unordered_map<std::string, Skills::PassiveSkillMetadata>
    passiveSkillMetadataCache_;
std::atomic<std::size_t> passiveClassificationCompleted_{0};
std::atomic<std::size_t> passiveClassificationTotal_{0};
bool hadUsablePassiveClassificationBeforeRefresh_{};
std::chrono::microseconds passiveClassificationElapsed_{};
std::size_t passiveClassificationTicks_{};
```

这些字段只保存标准库数据。原子计数仅用于 GUI 展示进度，避免每个批次复制并发布完整技能目录。

- [ ] **Step 3: 在技能目录成功刷新后启动分类**

把当前刷新代码拆成明确的辅助函数：

```cpp
void refresh_skill_catalog_on_game_thread()
{
    const auto previous = skillRuntimeSnapshot_.catalog;
    auto refreshed = skillGateway_.load_catalog();
    const bool passiveRefreshSucceeded = refreshed.passive.ready;

    skillRuntimeSnapshot_.catalog =
        Skills::with_catalog_fallback(previous, std::move(refreshed));

    if (passiveRefreshSucceeded)
    {
        hadUsablePassiveClassificationBeforeRefresh_ =
            previous.passiveClassification.ready;
        Skills::apply_passive_metadata(
            skillRuntimeSnapshot_.catalog.passive.skills,
            passiveSkillMetadataCache_);
        passiveClassificationJob_.start(
            skillRuntimeSnapshot_.catalog.passive.skills,
            passiveSkillMetadataCache_);
        skillRuntimeSnapshot_.catalog.passiveClassification =
            passiveClassificationJob_.status();
        passiveClassificationCompleted_.store(
            passiveClassificationJob_.status().completed,
            std::memory_order_relaxed);
        passiveClassificationTotal_.store(
            passiveClassificationJob_.status().total,
            std::memory_order_relaxed);
        passiveClassificationElapsed_ = {};
        passiveClassificationTicks_ = 0;
    }

    skillSnapshotDirty_ = true;
}
```

要求：

- 被动目录刷新失败时不得启动新任务，继续保留 `with_catalog_fallback()` 返回的旧分类；
- 主动目录成功/失败仍独立处理；
- 全部 ID 都命中成功缓存时，任务立即 `ready`，无需调用反射网关；
- 启动时发布一次未分类/缓存合并后的快照，使“全部”可立即使用。

- [ ] **Step 4: 每个 EngineTick 推进一个有界批次**

新增：

```cpp
void advance_passive_classification_on_game_thread()
{
    if (!passiveClassificationJob_.active())
    {
        return;
    }

    const auto ids =
        passiveClassificationJob_.next_batch(kPassiveMetadataBatchSize);
    const auto batch = skillGateway_.load_passive_skill_metadata_batch(
        ids, kPassiveMetadataBatchSize, kPassiveMetadataBudget);
    passiveClassificationElapsed_ += batch.elapsed;
    ++passiveClassificationTicks_;

    if (!batch.error.empty())
    {
        passiveClassificationJob_.fail(batch.error);
        finish_passive_classification_on_game_thread();
        return;
    }

    if (batch.entries.empty())
    {
        passiveClassificationJob_.fail(
            "passive metadata batch made no progress");
        finish_passive_classification_on_game_thread();
        return;
    }

    if (!passiveClassificationJob_.complete_batch(
            batch.entries, passiveSkillMetadataCache_))
    {
        finish_passive_classification_on_game_thread();
        return;
    }

    const auto status = passiveClassificationJob_.status();
    passiveClassificationCompleted_.store(
        status.completed, std::memory_order_relaxed);
    if (!passiveClassificationJob_.active())
    {
        finish_passive_classification_on_game_thread();
    }
}
```

在现有目录刷新判断之后调用它。不得在 GUI 回调或 `on_update()` 中调用。

- [ ] **Step 5: 完成或失败时一次性发布分类目录**

`finish_passive_classification_on_game_thread()`：

1. 重新用成功缓存填充当前被动技能数组；
2. 获取任务最终状态；
3. 若结构性失败且刷新前已有可用分类，把 `ready` 恢复为 `true`，保留错误字符串；已缓存的旧技能仍可按类别过滤，新/未知技能仅在“全部”；
4. 若首次分类结构性失败，保持 `ready == false`；
5. 更新完成/总数原子；
6. 设置 `skillSnapshotDirty_ = true`，只在这里发布最终分类元数据；
7. 输出一条聚合日志，包含成功/未知数量、EngineTick 次数、累计耗时和结构错误；禁止逐 ID/逐帧日志。

日志示例：

```text
PalworldEditor passive classification completed: 312/315 known, 41 ticks, 18.4 ms
```

或：

```text
PalworldEditor passive classification failed after 64/315: missing Rank property
```

- [ ] **Step 6: 在 LoadMap 前取消任务但保留缓存**

在 `begin_world_transition()` 中：

```cpp
passiveClassificationJob_.cancel();
passiveClassificationCompleted_.store(0, std::memory_order_relaxed);
passiveClassificationTotal_.store(0, std::memory_order_relaxed);
hadUsablePassiveClassificationBeforeRefresh_ = false;
```

保持现有世界代次、待处理编辑请求和 GUI 快照清理逻辑不变；不得调用
`passiveSkillMetadataCache_.clear()`。

`finish_world_transition()` 继续通过现有 `wantRefreshSkillCatalog_` 安排世界就绪后的目录刷新，不直接做反射。

- [ ] **Step 7: 构建并运行自动化测试**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditor PalworldEditorTests
ctest --test-dir build --output-on-failure
```

Expected: DLL 构建成功，全部测试通过。

- [ ] **Step 8: 提交调度实现**

```powershell
git add mods/PalworldEditor/src/dllmain.cpp mods/PalworldEditor/tests/skill_editor_tests.cpp
git commit -m "feat: schedule passive classification on engine ticks"
```

---

## Task 5: 实现两级被动技能选择器、颜色和完整搜索

**Files:**

- Modify: `mods/PalworldEditor/src/dllmain.cpp`

- [ ] **Step 1: 添加类别标签与颜色映射**

在 GUI 私有辅助区加入：

```cpp
[[nodiscard]] constexpr auto passive_category_label(
    const std::optional<Skills::PassiveSkillCategory> category) -> const char*
{
    if (!category.has_value())
    {
        return "全部";
    }
    switch (*category)
    {
    case Skills::PassiveSkillCategory::normal:
        return "普通";
    case Skills::PassiveSkillCategory::rare:
        return "稀有";
    case Skills::PassiveSkillCategory::premium:
        return "极品";
    case Skills::PassiveSkillCategory::legendary:
        return "传说";
    case Skills::PassiveSkillCategory::negative:
        return "负面";
    }
    return "全部";
}
```

颜色使用清晰但不过曝的 ImGui RGBA：

```cpp
[[nodiscard]] constexpr auto passive_category_color(
    Skills::PassiveSkillCategory category) -> ImVec4
{
    switch (category)
    {
    case Skills::PassiveSkillCategory::normal:
        return {0.92F, 0.92F, 0.92F, 1.0F};
    case Skills::PassiveSkillCategory::rare:
        return {1.0F, 0.82F, 0.20F, 1.0F};
    case Skills::PassiveSkillCategory::premium:
        return {0.30F, 0.65F, 1.0F, 1.0F};
    case Skills::PassiveSkillCategory::legendary:
        return {0.72F, 0.40F, 1.0F, 1.0F};
    case Skills::PassiveSkillCategory::negative:
        return {1.0F, 0.30F, 0.30F, 1.0F};
    }
    return {1.0F, 1.0F, 1.0F, 1.0F};
}
```

- [ ] **Step 2: 把被动选择状态替换为专用状态对象**

成员替换：

```cpp
Skills::PassiveSkillPickerState passivePickerState_;
```

逐处替换原 `passiveChoice_`：

- 提交新增/替换请求时读取 `passivePickerState_.selected`；
- 打开新增或替换操作时只调用 `clear_selection()`，保留用户当前类别；
- 取消当前编辑时只清选择；
- `reset_skill_editor_ui()` 调用 `passivePickerState_.reset()`，用于目标/世界真正切换时恢复“全部”；
- 搜索缓冲区 `passiveSearch_` 保持原字段，不因 `set_category()` 改变。

- [ ] **Step 3: 在技能下拉框前渲染类别下拉框**

新增 `render_passive_category_picker()`：

- 顺序固定为：全部、普通、稀有、极品、传说、负面；
- “全部”始终可选；
- `snapshot.catalog.passiveClassification.ready == false` 时，五个具体类别以
  `ImGui::BeginDisabled()` 包裹；
- 选择变化时调用 `passivePickerState_.set_category(...)`；
- 每个具体类别文本使用对应颜色；
- 类别未就绪时，在同一行或下一行显示：
  `正在读取被动技能分类：<completed>/<total>`；
- `completed` 和 `total` 直接读取原子字段，不触发目录快照复制；
- 有结构错误时显示黄色/红色警告；若旧分类仍可用，文案明确说明“正在使用上一次成功分类”。

- [ ] **Step 4: 新增被动技能专用下拉框并保留搜索**

保留现有 `render_skill_picker()` 给主动技能。新增
`render_passive_skill_picker()`，内部调用：

```cpp
const auto filtered = Skills::filter_passive_skills(
    snapshot.catalog.passive.skills,
    passivePickerState_.category,
    passiveSearch_,
    excludedPassiveIds);
```

要求：

- 搜索输入框仍放在技能 combo 内，输入方式和现有实现一致；
- `matches_skill()` 同时搜索 `localizedName` 和 Raw ID；
- 过滤结果标签继续使用推荐格式 `中文名 [RawId]`；
- `Selectable` 文本按技能自身 `passiveMetadata->category` 着色；
- 未知元数据在“全部”中使用默认文本色；
- 当前选择预览按技能类别着色；
- 若已选技能因为当前搜索或排除条件不再可见，不自动改写其 ID；只有类别变化按已确认规则清空选择。

- [ ] **Step 5: 为当前已装备被动技能增加同一套颜色**

显示当前技能列表时通过 Raw ID 查找对应 `SkillOption`：

```cpp
[[nodiscard]] auto find_skill_option(
    std::span<const Skills::SkillOption> options,
    std::string_view id) -> const Skills::SkillOption*;
```

有元数据时使用 `ImGui::TextColored()`；未知时沿用普通 `ImGui::TextUnformatted()`。
删除/替换按钮和请求参数不变。

- [ ] **Step 6: 格式检查、构建和测试**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor PalworldEditorTests
ctest --test-dir build --output-on-failure
```

Expected: 格式检查、DLL 构建和测试全部通过。

- [ ] **Step 7: 提交 GUI**

```powershell
git add mods/PalworldEditor/src/dllmain.cpp
git commit -m "feat: add categorized passive skill picker"
```

---

## Task 6: 更新版本、文档并执行完整验证

**Files:**

- Modify: `mods/PalworldEditor/src/dllmain.cpp`
- Modify: `README.md`
- Modify: `AGENTS.md`
- Modify: `CLAUDE.md`
- Modify if present: `mods/PalworldEditor/README.md`

- [ ] **Step 1: 将 mod 版本更新为 1.6.5**

更新 `dllmain.cpp` 中所有一致性版本位置：

- mod 元数据版本；
- GUI 标题；
-加载日志。

搜索确认没有遗漏：

```powershell
rg -n "1\.6\.4|1\.6\.5" README.md AGENTS.md CLAUDE.md mods/PalworldEditor
```

- [ ] **Step 2: 更新用户文档**

README 文档必须说明：

- 新增与替换被动技能时先选类别，再选技能；
- 六个类别名称及颜色；
- “全部”包含暂时无法分类的技能；
- 中文名和 Raw ID 搜索在所有类别中都继续支持；
- 首次分类会在技能目录加载后以小批次后台完成；
- 手动刷新会复用成功缓存，只重试新技能和先前失败技能。

- [ ] **Step 3: 更新维护者架构文档**

`AGENTS.md` 与 `CLAUDE.md` 同步写明：

- 分类来源是 `GetSkillData` 的 `Rank` 与 `AddWorldTreePal`；
- 精确分类规则；
- 8 ID/EngineTick 与 500 微秒软预算；
- 只在目录刷新后运行；
- 成功缓存生命周期和 LoadMap 取消规则；
- 分类完成前仅“全部”可用；
- 结构性失败的旧快照回退；
- 搜索和主动技能目录不受影响；
- 1.6.5 的行为变更。

- [ ] **Step 4: 执行完整仓库验证**

在 VS 2022 x64 开发者环境运行：

```powershell
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor PalworldEditorTests PalworldEditorBaseResourceSharingTests
ctest --test-dir build --output-on-failure
git diff --check
git status --short
```

Expected:

- 三个构建目标成功；
- CTest 全部通过；
- `git diff --check` 无输出；
- `git status --short` 只显示本任务预期文档/版本变更。

- [ ] **Step 5: 执行游戏内端到端验证**

构建并部署：

```powershell
cmake --build --preset ninja-msvc-x64 --target deploy
```

依次验证：

1. 冷启动游戏，进入存档前无技能反射崩溃；
2. Common 主背包就绪后技能目录自动加载；
3. 分类加载期间“全部”可用，具体类别禁用，进度持续前进；
4. 分类完成后六个类别可切换；
5. 普通/稀有/极品/传说/负面分别呈现白/黄/蓝/紫/红；
6. 类别内中文名搜索正常；
7. 类别内 Raw ID 搜索正常；
8. 切换类别会清空待选择技能但保留搜索文字；
9. 新增被动技能成功；
10. 替换被动技能成功；
11. 删除被动技能成功；
12. 主动技能选择、搜索、装备、替换和清空行为无变化；
13. 四词条预设行为无变化；
14. 点击“刷新技能列表”不崩溃，已成功分类项直接复用缓存；
15. 退出并重进存档时未完成任务被取消，重新进入后可正常分类；
16. 数字键切换不会改变已锁定目标，写入前 GUID 校验仍生效；
17. UE4SS 日志每次任务只有完成或失败聚合记录，没有逐帧刷屏；
18. 使用 UE4SS/游戏帧时间图对比分类前后，确认无持续性丢帧或明显尖峰。

- [ ] **Step 6: 提交版本和文档**

```powershell
git add README.md AGENTS.md CLAUDE.md mods/PalworldEditor/src/dllmain.cpp
git add mods/PalworldEditor/README.md
git commit -m "docs: document passive skill categories"
```

若 `mods/PalworldEditor/README.md` 不存在，不执行第二条 `git add`，不得为了满足命令而新建重复文档。

- [ ] **Step 7: 最终检查提交范围**

```powershell
git status --short
git log --oneline --decorate -7
git diff main...HEAD --stat
git diff main...HEAD --check
```

Expected: 工作树干净；提交仅包含设计文档、实现计划、被动技能分类实现、测试、版本与相关文档。
