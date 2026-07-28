# 帕鲁属性编辑器 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在"选择当前帕鲁"流程中新增等级（1–80）、个体值（HP/攻击/防御，0–255，突破 100 上限）和亲密度（rank 0–10）的编辑，全部通过游戏线程按需直接写 `SaveParameter` 字段实现，不引入逐帧或后台工作。

**Architecture:** 新增独立的 `pal_stats` 纯 C++ 领域层（值类型、请求、快照、clamp、FIFO 队列，可单测）和 `PalStatGateway` 反射适配层（`read_stats` / `apply_stat_edit`，导航 `PalIndividualCharacterParameter.SaveParameter` 结构体字段）。`dllmain.cpp` 新增平行 `statQueue_`，把 `PalStatSnapshot` 并入既有 `SkillEditorSnapshot` 复用同一发布机制，在 `game_thread_tick` 里按需消费、在 `begin_world_transition` 清空；GUI 新增 `render_pal_stats`。复用既有目标解析、GUID/代次重校验和 LoadMap 安全模型，不触碰技能管线。

**Tech Stack:** C++23、UE4SS 反射 API（`FStructProperty`/`FByteProperty`/`FIntProperty`、`ProcessEvent`）、ImGui、CMake/Ninja、现有无 Unreal 依赖的 `PalworldEditorTests`。

## Global Constraints

- 数值范围固定：等级 `1–80`；个体值 `Talent_HP`/`Talent_Shot`/`Talent_Defense` 各 `0–255`（uint8 存储，突破游戏 100 上限）；亲密度 rank `0–10`。`Talent_Melee` 是 `Transient`（不存盘），不编辑。
- 所有反射只在游戏线程（`game_thread_tick`）按需执行：仅在"目标选中确认"、"Apply 后读回"时读属性；仅在用户点击"应用属性修改"时写。无逐帧任务、无后台线程、无 `FindAllOf` 扫描（`PalDatabaseCharacterParameter` 阈值查询用 `FindFirstOf`，每次 Apply 至多一次）。
- 不缓存跨帧 `UObject*`/`UFunction*`/`FProperty*`/参数缓冲区；每个 `ProcessEvent` 后允许目标失效，写前重新校验。
- 请求只携带 `targetGeneration`/`worldGeneration` 纯值；跨世界（LoadMap）请求自动失效并在 `begin_world_transition` 清空队列。
- 生效时机：写入存档后持久化；当前帕鲁面板数值在重新召唤或重载存档后刷新，不调用属性重算入口。
- 写等级/个体值用直接属性写（`SetPropertyValueInContainer`，与现有 `StackCount` 同模式）；亲密度上升用 `AddFriendShip(delta, true)`，下降直接写 `FriendshipPoint`，阈值来自 `PalDatabaseCharacterParameter::GetFriendshipRequiredPointByRank`。
- 不改变技能编辑、资源共享、目标锁定规则与四词条预设。
- 命名：领域命名空间 `pal_stats`；目标句柄 `PalStatTarget = std::uintptr_t`（独立于 `skill_editor::SkillTarget`，避免跨模块耦合）。

---

## File Structure

- **Create:** `mods/PalworldEditor/inc/pal_stats/pal_stat_editor.hpp` — 纯 C++ 领域：`PalStatValues`/`PalStatEditRequest`/`PalStatSnapshot`/`PalStatEditQueue`/范围常量/clamp。
- **Create:** `mods/PalworldEditor/inc/pal_stats/pal_stats.hpp` — `PalStatGateway` 反射网关声明（`is_valid`/`read_stats`/`apply_stat_edit`）。
- **Create:** `mods/PalworldEditor/src/pal_stats.cpp` — 网关实现：`SaveParameter` 嵌套结构体导航 + `GetLevel`/`GetFriendshipRank`/`GetFriendshipPoint`/`AddFriendShip` + `PalDatabaseCharacterParameter::GetFriendshipRequiredPointByRank`。
- **Modify:** `mods/PalworldEditor/CMakeLists.txt` — 把 `src/pal_stats.cpp` 加入 DLL 源。
- **Modify:** `mods/PalworldEditor/src/dllmain.cpp` — 网关/队列成员、`SkillEditorSnapshot.palStat` 字段、`game_thread_tick` 消费 stat 队列、选中/Apply 后读属性、`begin_world_transition` 清空、`render_pal_stats` GUI、`reset_skill_editor_ui` 重置输入。
- **Modify:** `mods/PalworldEditor/tests/skill_editor_tests.cpp` — 新增 `pal_stats` clamp/校验纯 C++ 测试。
- **Modify:** `README.md`、`AGENTS.md`、`CLAUDE.md` — 版本 1.6.6 与属性编辑文档。

---

## Task 1: 建立纯 C++ 属性领域模型、范围 clamp 与请求队列

**Files:**
- Create: `mods/PalworldEditor/inc/pal_stats/pal_stat_editor.hpp`
- Modify: `mods/PalworldEditor/tests/skill_editor_tests.cpp`

**Interfaces:**
- Produces: `pal_stats::PalStatValues`、`pal_stats::PalStatEditRequest`、`pal_stats::PalStatSnapshot`、`pal_stats::PalStatEditQueue`、`pal_stats::PalStatTarget`、`pal_stats::clamp_level`/`clamp_talent`/`clamp_friendship_rank`/`has_any_change`、范围常量。Task 2 的网关与 Task 3 的 dllmain 都依赖这些类型。

- [ ] **Step 1: 先写 clamp/校验的失败测试**

在 `skill_editor_tests.cpp` 现有 `main()` 之外新增以下测试函数，并在 `main()` 中按现有 `test_*()` 调用列表的样式追加调用：

```cpp
void test_pal_stat_clamp_respects_policy_bounds() {
    using namespace pal_stats;
    // 等级 1–80
    CHECK(clamp_level(0) == 1);
    CHECK(clamp_level(1) == 1);
    CHECK(clamp_level(50) == 50);
    CHECK(clamp_level(80) == 80);
    CHECK(clamp_level(81) == 80);
    CHECK(clamp_level(255) == 80);
    // 个体值 0–255（突破游戏 100 上限）
    CHECK(clamp_talent(-5) == 0);
    CHECK(clamp_talent(0) == 0);
    CHECK(clamp_talent(100) == 100);
    CHECK(clamp_talent(255) == 255);
    CHECK(clamp_talent(256) == 255);
    // 亲密度 rank 0–10
    CHECK(clamp_friendship_rank(-1) == 0);
    CHECK(clamp_friendship_rank(0) == 0);
    CHECK(clamp_friendship_rank(10) == 10);
    CHECK(clamp_friendship_rank(11) == 10);
}

void test_pal_stat_values_detects_any_change() {
    using namespace pal_stats;
    CHECK(!has_any_change(PalStatValues{}));
    CHECK(has_any_change(PalStatValues{.level = 1}));
    CHECK(has_any_change(PalStatValues{.talentHp = 0}));
    CHECK(has_any_change(PalStatValues{.friendshipRank = 10}));
}
```

`main()` 追加（紧随现有同类调用）：

```cpp
    test_pal_stat_clamp_respects_policy_bounds();
    test_pal_stat_values_detects_any_change();
```

- [ ] **Step 2: 运行测试并确认编译失败**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorTests
```

Expected: 编译失败，提示 `pal_stats` 命名空间或 `clamp_level`/`PalStatValues` 等不存在。

- [ ] **Step 3: 实现领域头文件**

创建 `mods/PalworldEditor/inc/pal_stats/pal_stat_editor.hpp`：

```cpp
/**
 * @file pal_stat_editor.hpp
 * @brief 与 Unreal 解耦的帕鲁属性编辑领域模型、范围 clamp 与线程安全请求队列。
 * @details 本文件只依赖标准库；具体游戏读写由 `PalStatGateway` 实现。
 */
#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

/** @brief 提供帕鲁等级/个体值/亲密度编辑的纯值领域模型。 */
namespace pal_stats {
/** @brief 由非拥有 `UObject*` 编码的临时帕鲁目标句柄；使用前必须由网关校验。 */
using PalStatTarget = std::uintptr_t;

/** @brief 等级下限（不超过游戏满级，避免经验表空段）。 */
inline constexpr int kLevelMin = 1;
/** @brief 等级上限。 */
inline constexpr int kLevelMax = 80;
/** @brief 个体值下限。 */
inline constexpr int kTalentMin = 0;
/** @brief 个体值上限（uint8 存储，突破游戏 100 上限）。 */
inline constexpr int kTalentMax = 255;
/** @brief 亲密度 rank 下限。 */
inline constexpr int kFriendshipRankMin = 0;
/** @brief 亲密度 rank 上限。 */
inline constexpr int kFriendshipRankMax = 10;

/**
 * @brief 期望写入的属性值；空 `optional` 表示不改该项。
 */
struct PalStatValues {
    std::optional<int> level;          /**< 等级，clamp 到 `[1, 80]`。 */
    std::optional<int> talentHp;       /**< 个体值·HP（`Talent_HP`）。 */
    std::optional<int> talentShot;     /**< 个体值·攻击（`Talent_Shot`，远程）。 */
    std::optional<int> talentDefense;  /**< 个体值·防御（`Talent_Defense`）。 */
    std::optional<int> friendshipRank; /**< 亲密度 rank，clamp 到 `[0, 10]`。 */
};

/** @brief 由 UI 提交、等待游戏线程执行的一次属性编辑请求。 */
struct PalStatEditRequest {
    PalStatValues values;        /**< 期望写入的属性值。 */
    std::uint64_t targetGeneration{}; /**< GUI 提交时观察到的已确认目标代数。 */
    std::uint64_t worldGeneration{};  /**< GUI 提交时观察到的世界代次。 */
};

/** @brief 从游戏读取到的当前属性值，供 GUI 显示。 */
struct PalStatSnapshot {
    int level{};          /**< 当前等级。 */
    int talentHp{};       /**< 当前个体值·HP。 */
    int talentShot{};     /**< 当前个体值·攻击。 */
    int talentDefense{};  /**< 当前个体值·防御。 */
    int friendshipRank{}; /**< 当前亲密度 rank。 */
    int friendshipPoint{}; /**< 当前亲密度原始点数。 */
    bool readable{};      /**< 是否已成功读取过一次（目标选中后）。 */
};

/** @brief 把等级限制到 `[kLevelMin, kLevelMax]`。 */
[[nodiscard]] inline auto clamp_level(const int value) -> int {
    return std::clamp(value, kLevelMin, kLevelMax);
}
/** @brief 把个体值限制到 `[kTalentMin, kTalentMax]`。 */
[[nodiscard]] inline auto clamp_talent(const int value) -> int {
    return std::clamp(value, kTalentMin, kTalentMax);
}
/** @brief 把亲密度 rank 限制到 `[kFriendshipRankMin, kFriendshipRankMax]`。 */
[[nodiscard]] inline auto clamp_friendship_rank(const int value) -> int {
    return std::clamp(value, kFriendshipRankMin, kFriendshipRankMax);
}
/** @return `values` 是否至少设置了一个待写字段。 */
[[nodiscard]] inline auto has_any_change(const PalStatValues& values) -> bool {
    return values.level.has_value() || values.talentHp.has_value() ||
           values.talentShot.has_value() || values.talentDefense.has_value() ||
           values.friendshipRank.has_value();
}

/**
 * @brief 在线程安全 FIFO 中暂存 UI 提交的属性编辑请求。
 * @details 多生产者提交、唯一游戏线程按序消费；所有公开方法均在内部加锁。
 */
class PalStatEditQueue {
public:
    /** @brief 加锁后把请求追加到队尾。 */
    auto push(PalStatEditRequest request) -> void {
        const std::lock_guard lock(mutex_);
        requests_.push_back(std::move(request));
    }
    /** @return 加锁后从队首取出一个请求；队列为空时返回 `std::nullopt`。 */
    [[nodiscard]] auto try_pop() -> std::optional<PalStatEditRequest> {
        const std::lock_guard lock(mutex_);
        if (requests_.empty()) {
            return std::nullopt;
        }
        auto request = std::move(requests_.front());
        requests_.pop_front();
        return request;
    }
    /** @return 加锁后尚未处理的请求数量。 */
    [[nodiscard]] auto size() const -> std::size_t {
        const std::lock_guard lock(mutex_);
        return requests_.size();
    }
    /** @brief 加锁后丢弃全部待处理请求。 */
    auto clear() -> void {
        const std::lock_guard lock(mutex_);
        requests_.clear();
    }

private:
    mutable std::mutex mutex_;              /**< 保护 `requests_` 的唯一互斥量。 */
    std::deque<PalStatEditRequest> requests_; /**< 按提交顺序保存待执行请求。 */
};
}  // namespace pal_stats
```

在 `skill_editor_tests.cpp` 顶部包含该头文件（紧随现有 `#include <skills/...>` 之后，按字母序）：

```cpp
#include <pal_stats/pal_stat_editor.hpp>
```

- [ ] **Step 4: 运行测试并确认通过**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditorTests
ctest --test-dir build --output-on-failure -R SkillEditor
```

Expected: 新增测试与全部既有测试通过。

- [ ] **Step 5: 提交纯领域模型**

```powershell
git add mods/PalworldEditor/inc/pal_stats/pal_stat_editor.hpp mods/PalworldEditor/tests/skill_editor_tests.cpp
git commit -m "feat: add pal stat editor domain model"
```

---

## Task 2: 通过安全反射读写等级、个体值与亲密度

**Files:**
- Create: `mods/PalworldEditor/inc/pal_stats/pal_stats.hpp`
- Create: `mods/PalworldEditor/src/pal_stats.cpp`
- Modify: `mods/PalworldEditor/CMakeLists.txt`

**Interfaces:**
- Consumes: `pal_stats::PalStatTarget`/`PalStatEditRequest`/`PalStatValues`/`PalStatSnapshot`/clamp（来自 Task 1）；`pal_game::is_valid`（来自 `inc/game/pal_game.hpp`）。
- Produces: `pal_stats::PalStatGateway`，成员 `is_valid(PalStatTarget) const -> bool`、`read_stats(PalStatTarget) -> PalStatSnapshot`、`apply_stat_edit(PalStatTarget, const PalStatEditRequest&) -> bool`。Task 3 的 dllmain 持有一个实例并调用这三个方法。

- [ ] **Step 1: 声明网关接口**

创建 `mods/PalworldEditor/inc/pal_stats/pal_stats.hpp`：

```cpp
/**
 * @file pal_stats.hpp
 * @brief 声明把帕鲁属性编辑领域服务适配到 Palworld Unreal 反射接口的网关。
 */
#pragma once

#include <pal_stats/pal_stat_editor.hpp>

/** @brief 提供 Palworld 特定的帕鲁属性读取与写入能力。 */
namespace pal_stats {
/**
 * @brief 通过 `PalIndividualCharacterParameter.SaveParameter` 反射读写帕鲁属性。
 * @details 本类不拥有任何 Unreal 对象。所有成员函数都必须在游戏线程调用；
 *          `apply_stat_edit` 的布尔返回值只表示能否发起写入，调用方仍需重读确认。
 */
class PalStatGateway final {
public:
    /** @brief 检查目标句柄是否仍指向可访问的帕鲁 UObject。 */
    [[nodiscard]] auto is_valid(PalStatTarget target) const -> bool;

    /**
     * @brief 读取当前等级、三项个体值与亲密度 rank/point。
     * @return 从游戏反射读取的属性快照；目标失效或结构不可用时 `readable == false`。
     */
    [[nodiscard]] auto read_stats(PalStatTarget target) -> PalStatSnapshot;

    /**
     * @brief 按 `request.values` 写入各项属性；空 optional 跳过该项。
     * @retval true 目标与 `SaveParameter` 结构有效，已对每个设置项发起写入。
     * @retval false 目标失效或 `SaveParameter` 结构不可达。
     * @note 返回 `true` 不保证游戏立即刷新面板；按设计在重召唤/重载后可见。
     */
    auto apply_stat_edit(PalStatTarget target, const PalStatEditRequest& request) -> bool;
};
}  // namespace pal_stats
```

- [ ] **Step 2: 把源文件加入构建**

修改 `mods/PalworldEditor/CMakeLists.txt`，在 `add_library(${TARGET} SHARED ...)` 列表中、`src/pal_skills.cpp` 之后新增一行：

```cmake
    src/pal_stats.cpp
```

- [ ] **Step 3: 实现网关（结构解析 + 读 + 写）**

创建 `mods/PalworldEditor/src/pal_stats.cpp`：

```cpp
/**
 * @file pal_stats.cpp
 * @brief 实现帕鲁属性编辑网关：导航 `SaveParameter` 结构体并读写等级/个体值/亲密度。
 * @details 所有接口在游戏线程执行，所有 Unreal 裸指针均为非拥有观察指针，
 *          不跨调用缓存任何句柄或属性指针。
 */
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <game/pal_game.hpp>
#include <pal_stats/pal_stat_editor.hpp>
#include <pal_stats/pal_stats.hpp>

using namespace RC;
using namespace RC::Unreal;

namespace pal_stats {
namespace {
/** @brief 亲密度阈值查询失败时的保守回退点数（rank 0 对应 0 点）。 */
inline constexpr int kFriendshipPointFallback = 0;
/** @brief 把目标句柄还原为非拥有帕鲁 UObject，失效时返回 `nullptr`。 */
[[nodiscard]] auto to_pal(const PalStatTarget target) -> UObject* {
    auto* pal = reinterpret_cast<UObject*>(target);
    return pal_game::is_valid(pal) ? pal : nullptr;
}

/** @brief 调用无参、返回 `int32` 的 UFunction；目标/函数不可用时返回 `fallback`。 */
[[nodiscard]] auto invoke_int_return(UObject* object, const TCHAR* name,
                                     const int fallback = 0) -> int {
    if (!pal_game::is_valid(object)) {
        return fallback;
    }
    auto* const function = object->GetFunctionByNameInChain(name);
    if (function == nullptr) {
        return fallback;
    }
    struct Params {
        int32_t ReturnValue{};
    } params;
    object->ProcessEvent(function, &params);
    return params.ReturnValue;
}

/** @brief 取得帕鲁 `SaveParameter` 结构体内存指针与其 UStruct；不可达时返回 `nullptr`。 */
[[nodiscard]] auto save_parameter_slot(UObject* pal, FStructProperty*& outProperty)
    -> void* {
    auto* const prop = pal->GetPropertyByNameInChain(STR("SaveParameter"));
    outProperty = CastField<FStructProperty>(prop);
    if (outProperty == nullptr || outProperty->GetStruct().Get() == nullptr) {
        return nullptr;
    }
    return outProperty->ContainerPtrToValuePtr<void>(pal);
}

/** @brief 读取 `SaveParameter` 中一个 `uint8` 字段；缺失时返回 `fallback`。 */
[[nodiscard]] auto read_byte(const UStruct* rowStruct, void* saveParam, const TCHAR* name,
                             const int fallback = 0) -> int {
    auto* const prop = CastField<FByteProperty>(rowStruct->FindProperty(FName(name, FNAME_Find)));
    if (prop == nullptr) {
        return fallback;
    }
    return prop->GetPropertyValueInContainer(saveParam);
}

/** @brief 把 `value` 经 clamp 后写入 `SaveParameter` 中一个 `uint8` 字段。 */
auto write_byte(const UStruct* rowStruct, void* saveParam, const TCHAR* name, const int value,
                int (*clampFn)(int)) -> void {
    auto* const prop = CastField<FByteProperty>(rowStruct->FindProperty(FName(name, FNAME_Find)));
    if (prop != nullptr) {
        prop->SetPropertyValueInContainer(saveParam, static_cast<std::uint8_t>(clampFn(value)));
    }
}

/** @brief 取得 `PalDatabaseCharacterParameter` 单例；不可用时返回 `nullptr`。 */
[[nodiscard]] auto database() -> UObject* {
    return UObjectGlobals::FindFirstOf(STR("PalDatabaseCharacterParameter"));
}

/**
 * @brief 查询某亲密度 rank 所需的累计点数阈值。
 * @details 使用动态参数缓冲区匹配 `GetFriendshipRequiredPointByRank(int32, int32&)` 的 UFunction
 *          布局，避免手工对齐；查询失败时返回 `fallback`。
 */
[[nodiscard]] auto friendship_required_point(const int rank, const int fallback) -> int {
    auto* const db = database();
    auto* const function =
        db == nullptr ? nullptr : db->GetFunctionByNameInChain(STR("GetFriendshipRequiredPointByRank"));
    if (function == nullptr) {
        return fallback;
    }
    std::vector<std::byte> buffer(static_cast<std::size_t>(function->GetParmsSize()));
    function->InitializeStruct(buffer.data());
    struct DestroyGuard {
        UFunction* function{};
        void* params{};
        ~DestroyGuard() { function->DestroyStruct(params); }
    } guard{.function = function, .params = buffer.data()};

    auto* const rankProp =
        CastField<FIntProperty>(function->FindProperty(FName(STR("FriendshipRank"), FNAME_Find)));
    auto* const outProp =
        CastField<FIntProperty>(function->FindProperty(FName(STR("OutRequiredPoint"), FNAME_Find)));
    if (rankProp == nullptr || outProp == nullptr) {
        return fallback;
    }
    rankProp->SetPropertyValueInContainer(buffer.data(), static_cast<int32_t>(rank));
    db->ProcessEvent(function, buffer.data());
    return outProp->GetPropertyValueInContainer(buffer.data());
}

/** @brief 调用 `AddFriendShip(int32, bool)` 增加亲密度点数。 */
auto add_friendship(UObject* pal, const int delta) -> void {
    auto* const function = pal->GetFunctionByNameInChain(STR("AddFriendShip"));
    if (function == nullptr) {
        return;
    }
    struct Params {
        int32_t Value{};
        bool bApplyPassiveSkill{};
    } params{.Value = static_cast<int32_t>(delta), .bApplyPassiveSkill = true};
    pal->ProcessEvent(function, &params);
}
}  // namespace

auto PalStatGateway::is_valid(const PalStatTarget target) const -> bool {
    return to_pal(target) != nullptr;
}

auto PalStatGateway::read_stats(const PalStatTarget target) -> PalStatSnapshot {
    PalStatSnapshot snapshot;
    auto* const pal = to_pal(target);
    if (pal == nullptr) {
        return snapshot;
    }
    snapshot.level = invoke_int_return(pal, STR("GetLevel"));
    snapshot.friendshipRank = invoke_int_return(pal, STR("GetFriendshipRank"));
    snapshot.friendshipPoint = invoke_int_return(pal, STR("GetFriendshipPoint"));

    FStructProperty* saveProperty = nullptr;
    void* const saveParam = save_parameter_slot(pal, saveProperty);
    if (saveParam == nullptr) {
        return snapshot;
    }
    const auto* const rowStruct = saveProperty->GetStruct().Get();
    snapshot.talentHp = read_byte(rowStruct, saveParam, STR("Talent_HP"));
    snapshot.talentShot = read_byte(rowStruct, saveParam, STR("Talent_Shot"));
    snapshot.talentDefense = read_byte(rowStruct, saveParam, STR("Talent_Defense"));
    snapshot.readable = true;
    return snapshot;
}

auto PalStatGateway::apply_stat_edit(const PalStatTarget target, const PalStatEditRequest& request)
    -> bool {
    auto* const pal = to_pal(target);
    if (pal == nullptr) {
        return false;
    }
    FStructProperty* saveProperty = nullptr;
    void* const saveParam = save_parameter_slot(pal, saveProperty);
    if (saveParam == nullptr) {
        return false;
    }
    const auto* const rowStruct = saveProperty->GetStruct().Get();

    if (request.values.level.has_value()) {
        write_byte(rowStruct, saveParam, STR("Level"), *request.values.level, clamp_level);
    }
    if (request.values.talentHp.has_value()) {
        write_byte(rowStruct, saveParam, STR("Talent_HP"), *request.values.talentHp, clamp_talent);
    }
    if (request.values.talentShot.has_value()) {
        write_byte(rowStruct, saveParam, STR("Talent_Shot"), *request.values.talentShot, clamp_talent);
    }
    if (request.values.talentDefense.has_value()) {
        write_byte(rowStruct, saveParam, STR("Talent_Defense"), *request.values.talentDefense,
                   clamp_talent);
    }
    if (request.values.friendshipRank.has_value() && pal_game::is_valid(pal)) {
        const int targetRank = clamp_friendship_rank(*request.values.friendshipRank);
        const int currentRank = invoke_int_return(pal, STR("GetFriendshipRank"));
        const int requiredPoint = friendship_required_point(targetRank, kFriendshipPointFallback);
        if (targetRank >= currentRank) {
            const int currentPoint = invoke_int_return(pal, STR("GetFriendshipPoint"));
            add_friendship(pal, std::max(0, requiredPoint - currentPoint));
        } else {
            auto* const pointProp =
                CastField<FIntProperty>(rowStruct->FindProperty(FName(STR("FriendshipPoint"), FNAME_Find)));
            if (pointProp != nullptr) {
                pointProp->SetPropertyValueInContainer(saveParam,
                                                       static_cast<int32_t>(requiredPoint));
            }
        }
    }
    return true;
}
}  // namespace pal_stats
```

- [ ] **Step 4: 编译 DLL 验证反射类型与结构导航**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditor
```

Expected: `PalworldEditor.dll` 编译链接成功；无 `FStructProperty`/`FByteProperty`/`FIntProperty`/`FindProperty`/`ProcessEvent` API 错误。若 `GetFriendshipRequiredPointByRank` 的属性名（`FriendshipRank`/`OutRequiredPoint`）在当前 dump 下不同，以 `function->FindProperty` 实际命中的名字为准，不通过 reinterpret cast 绕开类型检查。

- [ ] **Step 5: 提交反射网关**

```powershell
git add mods/PalworldEditor/inc/pal_stats/pal_stats.hpp mods/PalworldEditor/src/pal_stats.cpp mods/PalworldEditor/CMakeLists.txt
git commit -m "feat: read and write pal stats via SaveParameter reflection"
```

---

## Task 3: 在 EngineTick 中接入属性队列、快照与 LoadMap 清空

**Files:**
- Modify: `mods/PalworldEditor/src/dllmain.cpp`
- Modify: `mods/PalworldEditor/tests/skill_editor_tests.cpp`（仅若新增了请求代次校验的纯函数测试；否则跳过）

**Interfaces:**
- Consumes: `pal_stats::PalStatGateway`、`pal_stats::PalStatEditQueue`、`pal_stats::PalStatEditRequest`、`pal_stats::PalStatSnapshot`、`pal_stats::has_any_change`（来自 Task 1/2）；既有 `pal_game::resolve_selected_otomo`、`skill_editor::SelectedTargetState`、`skill_editor::WorldSessionState`、`SkillEditorSnapshot`、`publish_skill_snapshot_if_dirty`。
- Produces: `PalworldEditorMod` 新增成员 `statGateway_`/`statQueue_`；`SkillEditorSnapshot` 新增 `palStat` 字段；游戏线程在选中确认与 stat Apply 后读属性并在 `begin_world_transition` 清空队列。Task 4 的 GUI 依赖 `snapshot.palStat` 与 `statQueue_`。

- [ ] **Step 1: 新增 include 与成员**

在 `dllmain.cpp` 既有 `#include <skills/world_session_state.hpp>` 之后按字母序加入：

```cpp
#include <pal_stats/pal_stat_editor.hpp>
#include <pal_stats/pal_stats.hpp>
```

在 `SkillEditorSnapshot` 结构体（含 `lastResult` 的那段）末尾、`targetConfirmedForWorld` 之前或之后新增一个字段：

```cpp
    pal_stats::PalStatSnapshot palStat; /**< 最近一次从游戏重读的实际属性值。 */
```

在类成员区（紧随 `pal_skills::PalSkillGateway skillGateway_;` 之后）新增：

```cpp
    /** @brief 在游戏线程执行帕鲁属性反射读写的无 UObject 所有权网关。 */
    pal_stats::PalStatGateway statGateway_;
    /** @brief GUI 生产、游戏线程 FIFO 消费的线程安全属性编辑请求队列。 */
    pal_stats::PalStatEditQueue statQueue_;
```

- [ ] **Step 2: 在选中确认后读取属性**

定位 `game_thread_tick` 中 `selectionRequested` 分支里写 `skillRuntimeSnapshot_.state = skillGateway_.read_state(...)` 的位置（紧跟其后的 `skillRuntimeSnapshot_.lastResult.clear();`），在该 `lastResult.clear()` 后补一行：

```cpp
                skillRuntimeSnapshot_.palStat =
                    statGateway_.read_stats(reinterpret_cast<pal_stats::PalStatTarget>(
                        resolvedPal->parameter));
```

并在同分支 `else`（选择失败）里、`skillRuntimeSnapshot_.lastResult = "选择失败：";` 之前补：

```cpp
                skillRuntimeSnapshot_.palStat = {};
```

- [ ] **Step 3: 在技能编辑块之后消费 stat 队列**

定位 `game_thread_tick` 中技能编辑结果处理块（`if (editRequest.has_value()) { ... }`，以 `skillRuntimeSnapshot_.lastResult = editResult->message;` 与 `skillSnapshotDirty_ = true;` 结尾）。在该块之后、`manualRefreshRequested` 之前，新增 stat 队列消费块（复用同一帧已解析的 `resolvedPal`/`resolution`/`selectedTarget_`/`worldSession_`，仅在目标代次与世界代次一致且目标已解析时执行）：

```cpp
        std::optional<pal_stats::PalStatEditRequest> statRequest;
        if (!selectionRequested) {
            statRequest = statQueue_.try_pop();
        }
        if (statRequest.has_value()) {
            const bool generationCurrent =
                selectedTarget_.generation() == statRequest->targetGeneration &&
                worldSession_.generation() == statRequest->worldGeneration &&
                resolvedPal.has_value() && resolution.resolved;
            const auto target = generationCurrent
                                    ? reinterpret_cast<pal_stats::PalStatTarget>(resolvedPal->parameter)
                                    : pal_stats::PalStatTarget{};
            if (generationCurrent && pal_stats::has_any_change(statRequest->values) &&
                statGateway_.is_valid(target)) {
                statGateway_.apply_stat_edit(target, *statRequest);
                skillRuntimeSnapshot_.palStat = statGateway_.read_stats(target);
            } else if (!generationCurrent) {
                statQueue_.clear();
            }
            skillSnapshotDirty_ = true;
        }
```

- [ ] **Step 4: 在 LoadMap 前清空 stat 队列**

定位 `begin_world_transition()`。在现有 `skillQueue_.clear();` 之后补：

```cpp
        statQueue_.clear();
```

并在同一函数内既有 `skillRuntimeSnapshot_.state = {};` 之后补：

```cpp
        skillRuntimeSnapshot_.palStat = {};
```

- [ ] **Step 5: 构建 DLL 与测试**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target PalworldEditor PalworldEditorTests
ctest --test-dir build --output-on-failure -R SkillEditor
```

Expected: DLL 构建成功，全部测试通过。

- [ ] **Step 6: 提交游戏线程接入**

```powershell
git add mods/PalworldEditor/src/dllmain.cpp
git commit -m "feat: schedule pal stat edits on engine ticks"
```

---

## Task 4: 实现属性编辑 GUI 与输入重置

**Files:**
- Modify: `mods/PalworldEditor/src/dllmain.cpp`

**Interfaces:**
- Consumes: `snapshot.palStat`、`statQueue_`、`snapshot.targetGeneration`/`worldGeneration`（来自 Task 3）；既有 `SkillEditorSnapshot`、`render_pal_editor`、`reset_skill_editor_ui`、`clamp(int,int,int)` 静态辅助。
- Produces: `render_pal_stats(self, snapshot, disabled)` 渲染函数；`render_pal_editor` 在主动技能区之后调用它。

- [ ] **Step 1: 新增 GUI 输入缓冲区成员与重置**

在类成员区（紧随 `char passiveSearch_[96]{};` 的同类 GUI 缓冲区附近）新增：

```cpp
    /** @brief 属性编辑输入缓冲区；只由 GUI 线程访问。 */
    int statLevelInput_{1};
    int statTalentHpInput_{0};
    int statTalentShotInput_{0};
    int statTalentDefenseInput_{0};
    int statFriendshipInput_{0};
```

在 `reset_skill_editor_ui(PalworldEditorMod* self)` 末尾补：

```cpp
        self->statLevelInput_ = 1;
        self->statTalentHpInput_ = 0;
        self->statTalentShotInput_ = 0;
        self->statTalentDefenseInput_ = 0;
        self->statFriendshipInput_ = 0;
```

- [ ] **Step 2: 实现 render_pal_stats**

在 `render_active_skills` 之后新增静态渲染函数：

```cpp
    /**
     * @brief 渲染等级、个体值与亲密度的编辑区，点击应用后入队一次属性请求。
     * @param[in,out] self 非空、非拥有的当前 mod 实例指针。
     * @param[in] snapshot 当前技能/属性编辑快照。
     * @param[in] mutationsDisabled 是否应禁用全部属性修改入口。
     * @warning 只在 GUI 线程调用。
     */
    static void render_pal_stats(PalworldEditorMod* self, const SkillEditorSnapshot& snapshot,
                                 const bool mutationsDisabled) {
        ImGui::TextUnformatted("属性修改");
        const auto& stats = snapshot.palStat;
        if (!stats.readable) {
            ImGui::TextDisabled("属性读取中或不可用");
            return;
        }
        ImGui::Text("当前：等级 %d / HP %d / 攻击 %d / 防御 %d / 亲密度 %d", stats.level,
                    stats.talentHp, stats.talentShot, stats.talentDefense, stats.friendshipRank);

        ImGui::SetNextItemWidth(120.0F);
        ImGui::DragInt("等级##stat-level", &self->statLevelInput_, 1.0F, 1, 80);
        ImGui::SetNextItemWidth(120.0F);
        ImGui::DragInt("个体值·HP##stat-hp", &self->statTalentHpInput_, 1.0F, 0, 255);
        ImGui::SetNextItemWidth(120.0F);
        ImGui::DragInt("个体值·攻击##stat-atk", &self->statTalentShotInput_, 1.0F, 0, 255);
        ImGui::SetNextItemWidth(120.0F);
        ImGui::DragInt("个体值·防御##stat-def", &self->statTalentDefenseInput_, 1.0F, 0, 255);
        ImGui::SetNextItemWidth(120.0F);
        ImGui::DragInt("亲密度##stat-friend", &self->statFriendshipInput_, 1.0F, 0, 10);

        ImGui::BeginDisabled(mutationsDisabled);
        if (ImGui::Button("应用属性修改")) {
            pal_stats::PalStatEditRequest request{
                .values = {.level = self->statLevelInput_,
                           .talentHp = self->statTalentHpInput_,
                           .talentShot = self->statTalentShotInput_,
                           .talentDefense = self->statTalentDefenseInput_,
                           .friendshipRank = self->statFriendshipInput_},
                .targetGeneration = snapshot.targetGeneration,
                .worldGeneration = snapshot.worldGeneration,
            };
            self->statQueue_.push(std::move(request));
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("(写入存档；重新召唤或重载后面板刷新)");
    }
```

- [ ] **Step 3: 在帕鲁编辑区挂接**

定位 `render_pal_editor` 末尾对 `render_active_skills(self, snapshot, pending || !editingReady);` 的调用，在其后补：

```cpp
        ImGui::Separator();
        render_pal_stats(self, snapshot, pending || !editingReady);
```

- [ ] **Step 4: 格式检查、构建与测试**

Run:

```powershell
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor PalworldEditorTests
ctest --test-dir build --output-on-failure -R SkillEditor
```

Expected: 格式检查、DLL 构建与测试全部通过。

- [ ] **Step 5: 提交 GUI**

```powershell
git add mods/PalworldEditor/src/dllmain.cpp
git commit -m "feat: add pal stat editor UI"
```

---

## Task 5: 更新版本至 1.6.6 并更新文档

**Files:**
- Modify: `mods/PalworldEditor/src/dllmain.cpp`
- Modify: `README.md`
- Modify: `AGENTS.md`
- Modify: `CLAUDE.md`

- [ ] **Step 1: 把 mod 版本更新为 1.6.6**

在 `dllmain.cpp` 中把以下三处的 `1.6.5`（若该分支已含）或 `1.6.4` 改为 `1.6.6`：`ModVersion`、加载日志 `PalworldEditor loaded (v1.6.x)`、GUI 窗口标题 `PalworldEditor v1.6.x`。搜索确认无遗漏：

```powershell
rg -n "1\.6\.[456]" mods/PalworldEditor README.md AGENTS.md CLAUDE.md
```

- [ ] **Step 2: 更新用户文档（README.md）**

在 README「功能」表新增一行：

```markdown
| **属性修改** | 选中帕鲁后修改等级（1–80）、个体值 HP/攻击/防御（0–255，可突破 100）与亲密度（0–10） |
```

在「帕鲁主动/被动技能」使用说明之后新增一节「帕鲁属性」，说明：选中帕鲁后显示当前 等级/个体值/亲密度；拖动输入后点"应用属性修改"；等级与亲密度不超过游戏上限，个体值可到 255；写入存档，重新召唤或重载后面板刷新；不修改 `MasteredWaza` 与伙伴技能。在「已知限制」追加一条 `1.6.6` 行为变更说明。

- [ ] **Step 3: 更新维护者文档（AGENTS.md、CLAUDE.md）**

在两份文档的架构分层与版本历史处写明：属性编辑来源是 `PalIndividualCharacterParameter.SaveParameter` 的 `Level`/`Talent_HP`/`Talent_Shot`/`Talent_Defense`/`FriendshipPoint`；等级 1–80、个体值 0–255、亲密度 rank 0–10；仅在目标选中与 Apply 后按需读写，无逐帧或后台工作；亲密度上升走 `AddFriendShip`、下降直接写点数，阈值来自 `PalDatabaseCharacterParameter::GetFriendshipRequiredPointByRank`；LoadMap 清空 stat 队列；`1.6.6` 的行为变更。

- [ ] **Step 4: 执行完整仓库验证**

在 VS x64 开发者环境运行：

```powershell
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor PalworldEditorTests PalworldEditorBaseResourceSharingTests
ctest --test-dir build --output-on-failure
git diff --check
git status --short
```

Expected: 三个构建目标成功；CTest 全部通过；`git diff --check` 无输出；`git status --short` 仅显示本任务预期变更。

- [ ] **Step 5: 提交版本与文档**

```powershell
git add mods/PalworldEditor/src/dllmain.cpp README.md AGENTS.md CLAUDE.md
git commit -m "docs: document pal stat editor"
```

---

## Task 6: 游戏内端到端验证

**Files:** 无（仅人工验证）

- [ ] **Step 1: 构建并部署**

```powershell
cmake --build --preset ninja-msvc-x64 --target deploy
```

- [ ] **Step 2: 依次验证**

1. 冷启动进入存档前无属性反射崩溃；
2. 选中一只帕鲁后"属性修改"区显示当前 等级/HP/攻击/防御/亲密度；
3. 把等级改为 80、点应用，重召唤后等级为 80；
4. 把某项个体值改为 255、点应用，重召唤后面板显示 255（确认 `攻击=Talent_Shot` 映射正确，若不符则修正字段名）；
5. 把亲密度改为 10、点应用，重召唤后面板显示 rank 10；
6. 把亲密度从 10 改回 3、点应用，重召唤后面板显示 rank 3（下降路径）；
7. 等级输入 81/0 被夹到 80/1；个体值输入 256 被夹到 255；
8. 切换数字键高亮目标后点应用，被游戏线程拒绝（目标代次不一致）；重新"选择当前帕鲁"后才生效；
9. 退出并重进存档时 stat 队列被清空，重新进入后可正常编辑；
10. 空闲等待至少 10 秒，UE4SS 日志无逐帧属性反射刷屏；
11. 主动/被动技能、四词条预设、资源共享行为无变化。

- [ ] **Step 3: 若亲密度阈值查询失败**

若第 5/6 步亲密度不生效，检查 `PalDatabaseCharacterParameter` 是否已加载、`GetFriendshipRequiredPointByRank` 属性名是否匹配；必要时把下降路径也改为读 `GetFriendshipRank(point)` 二分定位阈值，或在日志输出一次 `requiredPoint` 供排查。
