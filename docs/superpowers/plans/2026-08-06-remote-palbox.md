# 远程终端（Remote Palbox）实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 PalworldEditor mod 中实现任意地点按自定义快捷键打开原生 Palbox UI，配置持久化到 ini，支持 4 个门控开关。

**Architecture:** 遵循仓库"纯值层 + 游戏线程网关 + UI"模式。纯值层（配置解析、按键上升沿状态机、基地选择策略）可单测；游戏线程运行时在 EngineTick 每帧只做常量时间 WinAPI 检查（按键状态 + 前台窗口归属，各为固定少量调用），按键上升沿一次性执行"门控 → 基地解析 → `PalHUDService::Push` 打开原生 UI"；UI 通过互斥锁快照与请求队列与游戏线程通信。

**Tech Stack:** UE4SS C++23（反射 + ProcessEvent + `ForEachUObject`/`StaticFindObject`）、ImGui（UE4SS GUI）、WinAPI（`GetAsyncKeyState`/`GetForegroundWindow`/`GetModuleFileNameW`）、自定义 CHECK 测试框架（无 gtest）。

**Spec:** `docs/superpowers/specs/2026-08-06-remote-palbox-design.md`

## Global Constraints

（来自 spec 的硬约束，每个任务隐含包含本节）

- 全部反射调用：游戏线程单一入口、非拥有指针、`pal_game::is_valid` 校验、`CastField` 类型校验；跨帧不得持有 UObject 指针或 Unreal 数组地址。
- 结构故障（HUD 服务、`Push`、DispatchParameter 工厂、基地管理器任一不可用）→ 本世界代次内停用该域，LoadMap 后重新探测。
- 每帧开销固定为常量时间 WinAPI 检查（按键状态 + 前台窗口归属各为固定少量调用）；全部游戏逻辑仅在按键上升沿一次性执行；不使用 `FindAllOf`（Widget 类定位的 `ForEachUObject` 扫描仅首次触发执行一次，结果缓存为路径字符串）。
- 上升沿 300ms 防连点 + 进行中保护；触发执行超 2ms 打日志，连续 5 次超时 → 域停用。
- ini 解析：未知键忽略、损坏/缺失回退默认（HotkeyVk=74, DisableWhileMounted=true, DisableInDungeon=true, OnlyInsideBaseCircle=false, DisableDuringCombat=false）；键位只接受 1–255。
- 默认门控：地牢禁（`IsInStage`）、骑乘禁（`IsRiding`）；基地外/战斗默认可开。
- 不包含：聊天命令、地牢稳定性承诺、专用服务器支持。
- 每次提交前运行：`cmake --build --preset ninja-msvc-x64 --target format-check <涉及目标>`、`git diff --check`（全量验证在 Task 7）。

---

### Task 1: 纯值配置层 remote_palbox_config.hpp + 单测

**Files:**
- Create: `mods/PalworldEditor/inc/pal_remote_palbox/remote_palbox_config.hpp`
- Create: `mods/PalworldEditor/tests/remote_palbox_tests.cpp`（本任务只写 config 部分，Task 2/3 追加）
- Modify: `mods/PalworldEditor/CMakeLists.txt`（BUILD_TESTING 段追加独立测试可执行文件）

**Interfaces:**
- Produces:
  - `namespace pal_remote_palbox`
  - `struct RemotePalboxConfig { int hotkeyVk; bool disableWhileMounted; bool disableInDungeon; bool onlyInsideBaseCircle; bool disableDuringCombat; };`
  - `inline constexpr RemotePalboxConfig kDefaultRemotePalboxConfig{74, true, true, false, false};`（J 键 = VK 0x4A = 74）
  - `auto parse_remote_palbox_config(std::string_view content) -> RemotePalboxConfig;`（按行解析 `Key=Value`；未知键忽略；`HotkeyVk` 非 1–255 整数回退 74；其余键按 `true`/`false` 解析，非法回退默认）
  - `auto serialize_remote_palbox_config(const RemotePalboxConfig&) -> std::string;`（固定键序输出）

- [ ] **Step 1: 写失败测试（config 部分）**

`tests/remote_palbox_tests.cpp` 新建，测试 harness 与 `skill_editor_tests.cpp` 完全一致（CHECK 宏 + failures 计数 + main）：

```cpp
#include <iostream>
#include <string>
#include <string_view>

#include <pal_remote_palbox/remote_palbox_config.hpp>

namespace {
int failures = 0;
void check(const bool condition, const char* expression, const int line) {
    if (!condition) {
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++failures;
    }
}
}  // namespace

#define CHECK(expression) check((expression), #expression, __LINE__)

void test_parse_defaults_when_empty() {
    const auto config = pal_remote_palbox::parse_remote_palbox_config("");
    CHECK(config.hotkeyVk == 74);
    CHECK(config.disableWhileMounted);
    CHECK(config.disableInDungeon);
    CHECK(!config.onlyInsideBaseCircle);
    CHECK(!config.disableDuringCombat);
}

void test_parse_full_and_roundtrip() {
    const auto config = pal_remote_palbox::parse_remote_palbox_config(
        "HotkeyVk=75\nDisableWhileMounted=false\nDisableInDungeon=false\n"
        "OnlyInsideBaseCircle=true\nDisableDuringCombat=true\n");
    CHECK(config.hotkeyVk == 75);
    CHECK(!config.disableWhileMounted);
    CHECK(!config.disableInDungeon);
    CHECK(config.onlyInsideBaseCircle);
    CHECK(config.disableDuringCombat);
    const auto serialized = pal_remote_palbox::serialize_remote_palbox_config(config);
    const auto reparsed = pal_remote_palbox::parse_remote_palbox_config(serialized);
    CHECK(reparsed.hotkeyVk == 75);
    CHECK(!reparsed.disableWhileMounted);
    CHECK(!reparsed.disableInDungeon);
    CHECK(reparsed.onlyInsideBaseCircle);
    CHECK(reparsed.disableDuringCombat);
}

void test_parse_invalid_values_fall_back() {
    const auto config = pal_remote_palbox::parse_remote_palbox_config(
        "HotkeyVk=0\nHotkeyVk=300\nDisableInDungeon=maybe\nUnknownKey=1\n");
    CHECK(config.hotkeyVk == 74);
    CHECK(config.disableInDungeon);  // 非法值回退默认 true
}

void test_serialize_contains_all_keys() {
    const auto text = pal_remote_palbox::serialize_remote_palbox_config(
        pal_remote_palbox::kDefaultRemotePalboxConfig);
    CHECK(text.find("HotkeyVk=74") != std::string::npos);
    CHECK(text.find("DisableWhileMounted=true") != std::string::npos);
    CHECK(text.find("DisableInDungeon=true") != std::string::npos);
    CHECK(text.find("OnlyInsideBaseCircle=false") != std::string::npos);
    CHECK(text.find("DisableDuringCombat=false") != std::string::npos);
}

int main() {
    test_parse_defaults_when_empty();
    test_parse_full_and_roundtrip();
    test_parse_invalid_values_fall_back();
    test_serialize_contains_all_keys();
    if (failures > 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "remote_palbox config tests passed\n";
    return 0;
}
```

- [ ] **Step 2: 编译测试确认失败**

运行：`cmake --build --preset ninja-msvc-x64 --target PalworldEditorRemotePalboxTests`
预期：编译失败，找不到头文件 `pal_remote_palbox/remote_palbox_config.hpp`。

- [ ] **Step 3: 实现配置头**

`inc/pal_remote_palbox/remote_palbox_config.hpp`：

```cpp
/**
 * @file remote_palbox_config.hpp
 * @brief 远程终端 ini 配置的纯值解析与序列化。
 * @details 只处理字符串；文件 IO 由运行时层负责。
 */
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace pal_remote_palbox {

struct RemotePalboxConfig {
    int hotkeyVk{74};              /**< 打开快捷键 VK 码；默认 J（0x4A=74）。 */
    bool disableWhileMounted{true}; /**< 骑乘时禁用。 */
    bool disableInDungeon{true};    /**< 地牢内禁用。 */
    bool onlyInsideBaseCircle{false}; /**< 仅基地圈内可用。 */
    bool disableDuringCombat{false};  /**< 战斗中禁用。 */
};

inline constexpr RemotePalboxConfig kDefaultRemotePalboxConfig{};

[[nodiscard]] inline auto valid_hotkey_vk(const int vk) noexcept -> bool {
    return vk >= 1 && vk <= 255;
}

[[nodiscard]] inline auto parse_bool(const std::string_view value, const bool fallback) noexcept
    -> bool {
    if (value == "true") {
        return true;
    }
    if (value == "false") {
        return false;
    }
    return fallback;
}

[[nodiscard]] inline auto parse_remote_palbox_config(const std::string_view content)
    -> RemotePalboxConfig {
    RemotePalboxConfig config = kDefaultRemotePalboxConfig;
    std::size_t pos{};
    while (pos < content.size()) {
        const auto eol = content.find('\n', pos);
        const auto line =
            content.substr(pos, eol == std::string_view::npos ? content.size() - pos
                                                              : eol - pos);
        pos = eol == std::string_view::npos ? content.size() : eol + 1;
        const auto eq = line.find('=');
        if (eq == std::string_view::npos || eq == 0) {
            continue;
        }
        const auto key = line.substr(0, eq);
        const auto value = line.substr(eq + 1);
        if (key == "HotkeyVk") {
            const auto parsed = [&]() -> std::optional<int> {
                int result{};
                const auto [end, error] =
                    std::from_chars(value.data(), value.data() + value.size(), result);
                if (error != std::errc{} || end != value.data() + value.size()) {
                    return std::nullopt;
                }
                return result;
            }();
            if (parsed.has_value() && valid_hotkey_vk(*parsed)) {
                config.hotkeyVk = *parsed;
            }
        } else if (key == "DisableWhileMounted") {
            config.disableWhileMounted = parse_bool(value, config.disableWhileMounted);
        } else if (key == "DisableInDungeon") {
            config.disableInDungeon = parse_bool(value, config.disableInDungeon);
        } else if (key == "OnlyInsideBaseCircle") {
            config.onlyInsideBaseCircle = parse_bool(value, config.onlyInsideBaseCircle);
        } else if (key == "DisableDuringCombat") {
            config.disableDuringCombat = parse_bool(value, config.disableDuringCombat);
        }
        // 未知键忽略。
    }
    return config;
}

[[nodiscard]] inline auto serialize_remote_palbox_config(const RemotePalboxConfig& config)
    -> std::string {
    return "HotkeyVk=" + std::to_string(config.hotkeyVk) + "\n"
           "DisableWhileMounted=" + (config.disableWhileMounted ? "true" : "false") + "\n"
           "DisableInDungeon=" + (config.disableInDungeon ? "true" : "false") + "\n"
           "OnlyInsideBaseCircle=" + (config.onlyInsideBaseCircle ? "true" : "false") + "\n"
           "DisableDuringCombat=" + (config.disableDuringCombat ? "true" : "false") + "\n";
}
}  // namespace pal_remote_palbox
```

注意 `#include <charconv>` 需追加。

- [ ] **Step 4: CMake 增加测试目标**

`mods/PalworldEditor/CMakeLists.txt` 的 `if(BUILD_TESTING)` 段内、`PalworldEditorBaseResourceSharingTests` 块之后追加：

```cmake
    add_executable(PalworldEditorRemotePalboxTests
        tests/remote_palbox_tests.cpp
    )
    target_include_directories(PalworldEditorRemotePalboxTests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc)
    target_compile_features(PalworldEditorRemotePalboxTests PRIVATE cxx_std_23)
    if(MSVC)
        target_compile_options(PalworldEditorRemotePalboxTests
            PRIVATE /utf-8 /W4 /permissive-)
    endif()
    add_test(NAME PalworldEditor.RemotePalbox
        COMMAND PalworldEditorRemotePalboxTests)
```

- [ ] **Step 5: 编译并运行测试**

运行：`cmake --build --preset ninja-msvc-x64 --target PalworldEditorRemotePalboxTests && ctest --test-dir build -R RemotePalbox --output-on-failure`
预期：PASS，输出 `remote_palbox config tests passed`。

- [ ] **Step 6: 提交**

```bash
git add mods/PalworldEditor/inc/pal_remote_palbox/remote_palbox_config.hpp \
        mods/PalworldEditor/tests/remote_palbox_tests.cpp \
        mods/PalworldEditor/CMakeLists.txt
git commit -m "feat(remote-palbox): pure-value ini config parsing with tests"
```

---

### Task 2: 纯值按键上升沿状态机 + 单测

**Files:**
- Create: `mods/PalworldEditor/inc/pal_remote_palbox/remote_palbox.hpp`
- Modify: `mods/PalworldEditor/tests/remote_palbox_tests.cpp`（追加按键状态机测试 + main 调用）

**Interfaces:**
- Consumes: 无（纯 std）
- Produces（`namespace pal_remote_palbox`）:
  - `class HotkeyEdgeTrigger`
    - `auto update(std::chrono::steady_clock::time_point now, bool isPressed) -> bool`：上升沿（前帧未按、本帧按下）且距上次触发 ≥300ms 且不在进行中 → 触发并记录触发时刻、置进行中，返回 `true`；否则 `false`。
    - `auto end_trigger() noexcept -> void`：执行结束（无论成败）清除进行中，允许下次触发。
    - `auto in_flight() const noexcept -> bool`
    - `auto reset() noexcept -> void`：清空全部状态（LoadMap 用）。
  - `inline constexpr auto kHotkeyDebounce = std::chrono::milliseconds(300);`（**必须在类定义之前声明**，类体才能引用）

- [ ] **Step 1: 写失败测试**

`tests/remote_palbox_tests.cpp` 追加：

```cpp
#include <chrono>
#include <pal_remote_palbox/remote_palbox.hpp>

void test_edge_trigger_basic() {
    using clock = std::chrono::steady_clock;
    pal_remote_palbox::HotkeyEdgeTrigger trigger;
    const auto t0 = clock::time_point{};
    CHECK(!trigger.update(t0, false));                     // 未按不触发
    CHECK(!trigger.update(t0 + std::chrono::seconds(1), false));  // 仍未按
    CHECK(trigger.update(t0 + std::chrono::seconds(2), true));    // 上升沿触发
    CHECK(trigger.in_flight());
    CHECK(!trigger.update(t0 + std::chrono::seconds(3), true));   // 进行中忽略
    trigger.end_trigger();
    CHECK(!trigger.in_flight());
}

void test_edge_trigger_debounce_and_repeat() {
    using clock = std::chrono::steady_clock;
    pal_remote_palbox::HotkeyEdgeTrigger trigger;
    const auto t0 = clock::time_point{};
    CHECK(trigger.update(t0, true));
    trigger.end_trigger();
    CHECK(!trigger.update(t0 + std::chrono::milliseconds(200), false));  // 松开中
    // 300ms 内再次按下被防连点拦截
    CHECK(!trigger.update(t0 + std::chrono::milliseconds(299), true));
    // 松开后再按，且距上次触发已过 300ms → 再次触发
    CHECK(!trigger.update(t0 + std::chrono::milliseconds(300), false));
    CHECK(trigger.update(t0 + std::chrono::milliseconds(400), true));
    trigger.end_trigger();
}

void test_edge_trigger_held_key_does_not_repeat() {
    using clock = std::chrono::steady_clock;
    pal_remote_palbox::HotkeyEdgeTrigger trigger;
    const auto t0 = clock::time_point{};
    CHECK(trigger.update(t0, true));
    trigger.end_trigger();
    // 长按不触发（无下降沿）
    CHECK(!trigger.update(t0 + std::chrono::seconds(5), true));
    CHECK(!trigger.update(t0 + std::chrono::seconds(10), true));
    // 松开后再次按下，且已过防连点 → 触发
    CHECK(!trigger.update(t0 + std::chrono::seconds(11), false));
    CHECK(trigger.update(t0 + std::chrono::seconds(12), true));
    trigger.end_trigger();
}

void test_edge_trigger_reset() {
    using clock = std::chrono::steady_clock;
    pal_remote_palbox::HotkeyEdgeTrigger trigger;
    const auto t0 = clock::time_point{};
    CHECK(trigger.update(t0, true));
    trigger.reset();
    CHECK(!trigger.in_flight());
    CHECK(trigger.update(t0 + std::chrono::seconds(1), true));  // reset 后立即可触发
    trigger.end_trigger();
}
```

`main()` 中追加四个调用。

- [ ] **Step 2: 编译确认失败**

运行：`cmake --build --preset ninja-msvc-x64 --target PalworldEditorRemotePalboxTests`
预期：编译失败，`HotkeyEdgeTrigger` 未定义。

- [ ] **Step 3: 实现状态机**

`inc/pal_remote_palbox/remote_palbox.hpp`（文件头注释与 config 头一致）：

```cpp
inline constexpr auto kHotkeyDebounce = std::chrono::milliseconds(300);

/** @brief 远程终端按键上升沿状态机：防连点 + 进行中保护。 */
class HotkeyEdgeTrigger {
public:
    using clock = std::chrono::steady_clock;
    using time_point = clock::time_point;

    [[nodiscard]] auto update(const time_point now, const bool isPressed) -> bool {
        if (inFlight_) {
            return false;
        }
        if (!isPressed) {
            pressed_ = false;
            return false;
        }
        if (pressed_) {
            return false;  // 持续按下，等待下降沿
        }
        pressed_ = true;
        if (now - lastTrigger_ < kHotkeyDebounce) {
            return false;
        }
        lastTrigger_ = now;
        inFlight_ = true;
        return true;
    }

    auto end_trigger() noexcept -> void {
        inFlight_ = false;
    }

    [[nodiscard]] auto in_flight() const noexcept -> bool {
        return inFlight_;
    }

    auto reset() noexcept -> void {
        pressed_ = false;
        inFlight_ = false;
        lastTrigger_ = time_point{};
    }

private:
    bool pressed_{};
    bool inFlight_{};
    time_point lastTrigger_{};
};
```

注意：`kHotkeyDebounce` 需在类定义之前或之后定义均可（类内使用），确保文件内顺序为常量先于类或使用 `std::chrono::milliseconds(300)` 字面量并单独定义常量。若按上述顺序（常量在类后），把类内比较改为 `std::chrono::milliseconds(300)` 并保留常量供测试引用。

- [ ] **Step 4: 编译并运行测试**

运行：`cmake --build --preset ninja-msvc-x64 --target PalworldEditorRemotePalboxTests && ctest --test-dir build -R RemotePalbox --output-on-failure`
预期：PASS。

- [ ] **Step 5: 提交**

```bash
git add mods/PalworldEditor/inc/pal_remote_palbox/remote_palbox.hpp \
        mods/PalworldEditor/tests/remote_palbox_tests.cpp
git commit -m "feat(remote-palbox): hotkey edge trigger state machine with tests"
```

---

### Task 3: 纯值基地选择策略 + 单测

**Files:**
- Modify: `mods/PalworldEditor/inc/pal_remote_palbox/remote_palbox.hpp`（追加）
- Modify: `mods/PalworldEditor/tests/remote_palbox_tests.cpp`（追加）

**Interfaces:**
- Produces（`namespace pal_remote_palbox`）:
  - `struct BaseCampCandidate { std::string id; bool playerInside{}; double distanceSquared{}; };`
  - `auto select_remote_base_camp(std::span<const BaseCampCandidate> candidates) -> std::optional<std::size_t>`：优先返回第一个 `playerInside` 的索引；否则返回 `distanceSquared` 最小的索引；空返回 `std::nullopt`。

- [ ] **Step 1: 写失败测试**

```cpp
#include <span>
#include <vector>

void test_base_camp_selection_prefers_inside() {
    using pal_remote_palbox::BaseCampCandidate;
    using pal_remote_palbox::select_remote_base_camp;
    const std::vector<BaseCampCandidate> camps{
        {.id = "far", .playerInside = false, .distanceSquared = 100.0},
        {.id = "near-outside", .playerInside = false, .distanceSquared = 10.0},
        {.id = "inside", .playerInside = true, .distanceSquared = 5000.0},
    };
    const auto pick = select_remote_base_camp(camps);
    CHECK(pick.has_value());
    CHECK(*pick == 2);  // 玩家所在圈优先于更近的圈
}

void test_base_camp_selection_nearest_fallback() {
    using pal_remote_palbox::BaseCampCandidate;
    using pal_remote_palbox::select_remote_base_camp;
    const std::vector<BaseCampCandidate> camps{
        {.id = "far", .playerInside = false, .distanceSquared = 100.0},
        {.id = "near", .playerInside = false, .distanceSquared = 10.0},
    };
    const auto pick = select_remote_base_camp(camps);
    CHECK(pick.has_value());
    CHECK(*pick == 1);
}

void test_base_camp_selection_empty() {
    using pal_remote_palbox::select_remote_base_camp;
    const std::vector<pal_remote_palbox::BaseCampCandidate> camps{};
    CHECK(!select_remote_base_camp(camps).has_value());
}

void test_base_camp_selection_multiple_inside_takes_first() {
    using pal_remote_palbox::BaseCampCandidate;
    using pal_remote_palbox::select_remote_base_camp;
    const std::vector<BaseCampCandidate> camps{
        {.id = "a", .playerInside = true, .distanceSquared = 1.0},
        {.id = "b", .playerInside = true, .distanceSquared = 0.5},
    };
    const auto pick = select_remote_base_camp(camps);
    CHECK(pick.has_value());
    CHECK(*pick == 0);
}
```

`main()` 追加四个调用。

- [ ] **Step 2: 编译确认失败**

运行：`cmake --build --preset ninja-msvc-x64 --target PalworldEditorRemotePalboxTests`
预期：编译失败，`select_remote_base_camp` 未定义。

- [ ] **Step 3: 实现选择策略**

`inc/pal_remote_palbox/remote_palbox.hpp` 追加：

```cpp
/** @brief 基地选择候选：运行时解析后交给纯值选择器。 */
struct BaseCampCandidate {
    std::string id;                /**< 基地 ID 的字符串表示。 */
    bool playerInside{};           /**< 玩家是否位于该基地圈内。 */
    double distanceSquared{};      /**< 玩家到基地中心的平方距离（兜底排序用）。 */
};

/**
 * @brief 选择远程终端归属基地。
 * @details 策略：优先玩家当前所在圈（第一个 playerInside）；否则取最近基地；无候选返回空。
 */
[[nodiscard]] inline auto select_remote_base_camp(
    const std::span<const BaseCampCandidate> candidates) -> std::optional<std::size_t> {
    for (std::size_t index{}; index < candidates.size(); ++index) {
        if (candidates[index].playerInside) {
            return index;
        }
    }
    std::optional<std::size_t> nearest;
    for (std::size_t index{}; index < candidates.size(); ++index) {
        if (!nearest.has_value() ||
            candidates[index].distanceSquared < candidates[*nearest].distanceSquared) {
            nearest = index;
        }
    }
    return nearest;
}
```

文件需追加 `#include <optional>`、`#include <span>`、`#include <string>`（若已由 config 头传递则省略重复）。

- [ ] **Step 4: 编译并运行测试**

运行：`cmake --build --preset ninja-msvc-x64 --target PalworldEditorRemotePalboxTests && ctest --test-dir build -R RemotePalbox --output-on-failure`
预期：PASS。

- [ ] **Step 5: 提交**

```bash
git add mods/PalworldEditor/inc/pal_remote_palbox/remote_palbox.hpp \
        mods/PalworldEditor/tests/remote_palbox_tests.cpp
git commit -m "feat(remote-palbox): base camp selection strategy with tests"
```

---

### Task 4: 游戏线程运行时 remote_palbox_runtime.hpp/cpp

**Files:**
- Create: `mods/PalworldEditor/inc/pal_remote_palbox/remote_palbox_runtime.hpp`
- Create: `mods/PalworldEditor/src/pal_remote_palbox/remote_palbox_runtime.cpp`
- Modify: `mods/PalworldEditor/CMakeLists.txt`（主目标源列表追加 cpp）

**Interfaces:**
- Consumes: `parse_remote_palbox_config`/`serialize_remote_palbox_config`、`HotkeyEdgeTrigger`、`select_remote_base_camp`、`skill_editor::WorldSessionState`（`inc/skills/world_session_state.hpp`）
- Produces:
  - `enum class RemotePalboxTriggerResult { opened, blocked, noBase, unavailable, disabled };`
  - `struct RemotePalboxSnapshot { RemotePalboxConfig config; bool domainDisabled; std::string lastMessage; std::uint64_t openCount; std::uint64_t failCount; };`
  - `class RemotePalboxRuntime`（仅游戏线程调用，LoadMap 生命周期见下）：
    - `auto load_config(std::string_view iniPath) -> void`（读文件 → parse；读失败用默认值）
    - `auto set_config(RemotePalboxConfig) -> void`（GUI 修改后调用；同时写回 ini）
    - `auto tick(float deltaSeconds, const skill_editor::WorldSessionState& session) -> void`（每帧；WinAPI 轮询 + 上升沿 → `execute_trigger`）
    - `auto request_open() -> void`（GUI 测试按钮：置请求标志，下一 tick 走同一管线）
    - `auto begin_world_transition() -> void` / `auto finish_world_transition() -> void`（LoadMap 重置：触发状态、域停用、widget 路径缓存保留为字符串）
    - `auto snapshot() const -> RemotePalboxSnapshot`（互斥锁保护的纯值快照，供 GUI 线程）

- [ ] **Step 1: 运行时头文件**

`inc/pal_remote_palbox/remote_palbox_runtime.hpp`：按上述接口声明类；成员：`RemotePalboxConfig config_`、`HotkeyEdgeTrigger trigger_`、`bool requestedOpen_`、`bool domainDisabled_`、`bool domainProbed_`、`std::string widgetPath_`（跨世界保留）、`bool widgetPathResolved_`、`std::string lastMessage_`、计数、`std::chrono::steady_clock::time_point lastTriggerTime_`、连续超时计数、`mutable std::mutex snapshotMutex_`。`tick` 签名含 `deltaSeconds`（用于测试按钮节流，本版本仅作占位，实际节流由 trigger 的 300ms 防连点承担）。

- [ ] **Step 2: 实现核心管线（cpp）**

`src/pal_remote_palbox/remote_palbox_runtime.cpp`，实现顺序：

1. **按键轮询（每帧，唯一每帧开销）**：文件需 `#include <windows.h>`（`GetAsyncKeyState`/`GetForegroundWindow`/`GetCurrentProcessId`）与 `#include <chrono>`、`#include <string>`、`#include <vector>`、`#include <mutex>`。

```cpp
auto RemotePalboxRuntime::tick(const float deltaSeconds,
                               const skill_editor::WorldSessionState& session) -> void {
    const bool guiRequest = requestedOpen_.exchange(false);
    const bool pressed = (GetAsyncKeyState(config_.hotkeyVk) & 0x8000) != 0;
    const bool foreground = foreground_is_game();
    if (!guiRequest && (!foreground || !pressed ||
                        !trigger_.update(std::chrono::steady_clock::now(), pressed))) {
        return;
    }
    if (!session.can_access_unreal()) {
        return;
    }
    execute_trigger();
}
```

`foreground_is_game()`：`GetForegroundWindow()` 的线程进程 == `GetCurrentProcessId()`（不依赖窗口句柄）。

2. **门控**（`execute_trigger` 内，按序）：

```cpp
if (domainDisabled_) { return; }
if (!probe_domain()) { set_disabled("关键反射点不可用，本世界已停用远程终端"); return; }
if (config_.disableInDungeon && player_state_bool("IsInStage")) { note("地牢内已禁用"); return; }
if (config_.disableWhileMounted && player_controller_bool("IsRiding")) { note("骑乘中已禁用"); return; }
if (config_.disableDuringCombat && player_state_bool("IsInCombat")) { note("战斗中已禁用"); return; }
```

`player_state_bool`/`player_controller_bool`：`UObjectGlobals::FindFirstOf(STR("PalPlayerState"))` / `STR("PalPlayerController")`（与现有代码同款，见 `pal_base_resource_runtime.cpp:145` 的 `GetLocalPalPlayerController` 模式），`GetFunctionByNameInChain` + `ProcessEvent`，返回 `FBoolProperty` 的 `ReturnValue`。

**战斗检测为验证点（fail-open）**：dump 中 `PalPlayerState`/`PalPlayerController` 均未发现 `IsInCombat`/`IsInBattle`。实现时在 `PalPlayerState` 与 `PalPlayerController` 上依次探测 `IsInCombat`、`IsInBattle`、`IsInDungeonBattle` 候选函数名；全部不可用 → 该门控视为 false（fail-open，战斗不拦截）。默认开关为关，风险低；验证结果回填设计文档。
> **已过时（superseded）**：战斗门控最终实现为 fail-closed——启用时状态不可读即拦截
> （`remote_palbox_runtime.cpp` 的 `pal_game::player_in_battle_mode` 返回空 → `blocked`）。

3. **基地解析**（镜像 `pal_base_resource_runtime.cpp:157-216` 的 `read_base_ids`/`try_get_base_model`）：

- 世界上下文：`UObjectGlobals::FindFirstOf(pal_game::kInventoryClassName)`；
- `PalUtility:GetLocalPalPlayerController` → controller；
- `PalBaseCampManager` 获取：`FindFirstOf(STR("PalBaseCampManager"))`（资源分享契约同款）；
- `GetBaseCampIds(OutIds)` → 每个 FGuid：
  - `TryGetModel(BaseCampId, OutModel)` → model；
  - 读 `PalBaseCampModel` 属性：`ID`（FGuid，经 `FStructProperty` + `CopyCompleteValue` 到本地 FGuid）、`OwnerMapObjectInstanceId`（同）、`AreaRange`（FIntProperty？dump 为 `float AreaRange` → `FFloatProperty`）；
  - `playerInside`：仅当 `config_.onlyInsideBaseCircle` 开启时才计算（默认关可跳过距离计算）：玩家 Pawn 位置（controller `GetPawn` → `K2_GetActorLocation`）与基地中心（PalBox concrete model 位置，经 `PalMapObjectManager:FindConcreteModel(OwnerMapObjectInstanceId)`，见资源分享 `find_concrete_model`）的 `Size2D()` 平方 ≤ `AreaRange²`；
  - `distanceSquared`：同源计算，供兜底排序。

> **已过时（superseded）**：圈内判定不再使用据点模型 `AreaRange`（随据点等级膨胀，
> 大于视觉建造圈）。现行实现以世界设置 `BaseCampAreaRange`（`PalUtility:GetGameSetting`）
> 为唯一半径来源，且基地中心改读基地模型 `Transform.Translation`（物理 Palbox actor
> 未流加载也可用）；世界设置不可读时按拦截处理（fail-closed），不回退模型半径。
> 候选同时按本地公会（`GetGroupIdBelongTo`）过滤，归属不可读同样拦截。

4. **归属选择**：`select_remote_base_camp(candidates)`；无结果 → `noBase` + note("没有可用的已拥有基地")。

5. **Widget 类解析（首次触发执行一次，结果缓存为路径字符串）**：

```cpp
auto resolve_widget_path() -> bool {
    if (widgetPathResolved_) { return !widgetPath_.empty(); }
    widgetPathResolved_ = true;
    std::string found;
    UObjectGlobals::ForEachUObject([&](UObject* obj, int32_t, int32_t) -> LoopAction {
        if (!found.empty()) { return LoopAction::Stop; }
        if (obj->GetClassPrivate() != UClass::StaticClass()) { return LoopAction::Continue; }
        if (obj->GetName() == L"WBP_PalBox_C") {
            found = text_encoding::to_utf8(obj->GetPathName());
        }
        return LoopAction::Continue;
    });
    widgetPath_ = std::move(found);
    if (widgetPath_.empty()) {
        Output::send<LogLevel::Warning>(
            STR("PalworldEditor: WBP_PalBox_C not found; remote palbox disabled\n"));
    }
    return !widgetPath_.empty();
}
```

`UClass::StaticClass()` 与 `LoopAction::Stop` 语义以 `RE-UE4SS/deps/first/Unreal/include/Unreal/UObjectGlobals.hpp` 的 `ForEachUObject` 签名为准（回调返回 `LoopAction`，`Stop` 停止迭代）。后续触发用 `UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, widePath.c_str())` 重取（快，微秒级）。

6. **Push（核心写入）**：

```cpp
// PalHUDService 单例（FindFirstOf(STR("PalHUDService"))）。
// 1) CreateDispatchParameterForK2Node(WorldContextObject, UPalHUDDispatchParameter_PalBox::StaticClass())
//    → 返回 UPalHUDDispatchParameter_PalBox*；
// 2) 在返回对象上设置两个 FStructProperty："BaseCampId"、"OwnerMapObjectInstanceId"，
//    用 CopyCompleteValue 从本地 FGuid 拷贝（FGuid 布局 4×int32，见 UnrealCoreStructs.hpp）；
// 3) PalHUDService::Push(WidgetClass, param) → FGuid 返回值；
// 4) 返回值为非零 FGuid（任一字段非 0）→ opened；否则 failed。
```

> **已过时（superseded）**：Push 可异步完成——界面已入栈时返回值仍可能全零，直接判
> failed 会把成功误报为失败并可能错误停用域。现行实现：非零 → 立即确认成功；全零 →
> 记入 600ms 有界确认窗口（每 150ms 复查 HUD StackableUIWidgets/光标状态），窗口内
> 界面出现即确认成功，超时才记失败且不停用域（见 `remote_palbox_runtime.cpp` 的
> `pendingConfirm_`）。

参数缓冲区一律用 `std::vector<std::byte>(GetParmsSize())` + `InitializeStruct` + RAII `DestroyStruct`（镜像 `pal_skills.cpp` 的 `ParamsGuard` 模式）。`FGuid` 取自 `Unreal/UnrealCoreStructs.hpp`（`pal_game.hpp:240` 已使用，确认其布局为 4×int32 后直接值比较）。

7. **验证与计时**：`std::chrono::steady_clock` 计时整个 `execute_trigger`；超 2ms 打 `Warning` 日志；连续 5 次超时 → `set_disabled("触发耗时连续超限，已停用远程终端")`。

8. **结构故障域停用**：`probe_domain()` 检查（HUD 服务、Push、CreateDispatchParameterForK2Node、PalBaseCampManager、GetBaseCampIds、TryGetModel 六项反射点全部可得）→ 任一失败 `domainDisabled_ = true` 并记录消息。

9. **LoadMap**：`begin_world_transition()`：`trigger_.reset()`、`domainDisabled_ = false`、`domainProbed_ = false`、请求标志清零；`finish_world_transition()`：无操作（widgetPath_ 字符串缓存保留，跨世界有效）。

10. **日志**：每次触发结果用 `Output::send<LogLevel::Verbose>`（沿用诊断降级惯例）记录：`remote_palbox: result={...} base={} elapsed_us={}`。

- [ ] **Step 3: CMake 主目标追加源文件**

`mods/PalworldEditor/CMakeLists.txt` 主目标源列表（`src/pal_stats/stat_ui.cpp` 之后）追加：

```cmake
    src/pal_remote_palbox/remote_palbox_runtime.cpp
```

- [ ] **Step 4: 编译**

运行：`cmake --build --preset ninja-msvc-x64 --target PalworldEditor`
预期：编译通过（本任务不接入 dllmain，先保证独立编译）。

- [ ] **Step 5: 提交**

```bash
git add mods/PalworldEditor/inc/pal_remote_palbox/remote_palbox_runtime.hpp \
        mods/PalworldEditor/src/pal_remote_palbox/remote_palbox_runtime.cpp \
        mods/PalworldEditor/CMakeLists.txt
git commit -m "feat(remote-palbox): game-thread runtime pipeline (gate, base resolve, HUD push)"
```

---

### Task 5: GUI 远程终端区 remote_palbox_ui.cpp

**Files:**
- Create: `mods/PalworldEditor/src/pal_remote_palbox/remote_palbox_ui.cpp`
- Modify: `mods/PalworldEditor/CMakeLists.txt`（追加源文件）

**Interfaces:**
- Consumes: `RemotePalboxRuntime`（snapshot/request_open/set_config）、`RemotePalboxConfig`
- Produces: `auto render_remote_palbox(PalworldEditorMod* self) -> void`（在 `namespace pal_remote_palbox` 或 `editor_ui` 中，由 Task 6 的 render 分发调用）

- [ ] **Step 1: 实现 UI 渲染函数**

`src/pal_remote_palbox/remote_palbox_ui.cpp`：

- 布局（`ImGui::CollapsingHeader("Remote Palbox")` 内，风格对齐 `base_resource_ui.cpp`）：
  - **改键按钮**：`ImGui::Button("点击后按新键##rp-hotkey")` → 进入捕获模式（模块级 `bool capturing`）；捕获模式中遍历 `ImGui::GetIO()` 键盘按键，找到 `ImGui::IsKeyPressed(ImGuiKey_X)` 的键 → `imgui_key_to_vk()` 转换 → `self` 的 runtime `set_config` + 写回 ini；`Esc` 取消捕获。
  - `imgui_key_to_vk()` 辅助：`ImGuiKey_A..Z → VK_A..VK_Z`、`ImGuiKey_0..9 → VK_0..VK_9`、`ImGuiKey_F1..F12 → VK_F1..F12`、`ImGuiKey_Left/Right/Up/Down → VK_LEFT...`，其余键用 `ImGui::GetKeyIndex(ImGuiKey)` 作为 best-effort VK；无法映射（0）则忽略本次捕获。当前键位显示 `"VK {N} ({ImGui::GetKeyName})"` 或直接显示 VK 值。
  - **4 个开关**：`ImGui::Checkbox`，变更即 `set_config` 写回；`DisableInDungeon` 旁显示 `"(原版地牢内打开存在崩溃风险)"` 提示。
  - **测试按钮**：`ImGui::Button("立即打开终端##rp-test")` → `request_open()`。
  - **状态行**：`snapshot()` 显示：域停用状态（红色）、最近消息、openCount/failCount。
- 快照读取：`const std::lock_guard lock(...)` 或 runtime 的 `snapshot()`（内部加锁）。
- 所有修改经 `set_config`/`request_open`，不直接在 GUI 线程碰反射。

- [ ] **Step 2: CMake 追加源文件**

`mods/PalworldEditor/CMakeLists.txt` 主目标源列表追加 `src/pal_remote_palbox/remote_palbox_ui.cpp`。

- [ ] **Step 3: 编译**

运行：`cmake --build --preset ninja-msvc-x64 --target PalworldEditor`
预期：编译通过（render 函数暂未被引用，用 `[[maybe_unused]]` 或暂不声明在头；Task 6 接入）。

- [ ] **Step 4: 提交**

```bash
git add mods/PalworldEditor/src/pal_remote_palbox/remote_palbox_ui.cpp \
        mods/PalworldEditor/CMakeLists.txt
git commit -m "feat(remote-palbox): GUI section with hotkey capture, toggles, test button"
```

---

### Task 6: 集成 dllmain + mod_core + 渲染分发

**Files:**
- Modify: `mods/PalworldEditor/inc/mod/mod_core.hpp`（成员 + 渲染函数声明）
- Modify: `mods/PalworldEditor/src/mod/dllmain.cpp`（tick、LoadMap、渲染分发）

**Interfaces:**
- Consumes: `render_remote_palbox`、`RemotePalboxRuntime`、`skill_editor::WorldSessionState`

- [ ] **Step 1: mod_core.hpp 增加成员与声明**

- `#include <pal_remote_palbox/remote_palbox_runtime.hpp>`；
- 私有成员：`pal_remote_palbox::RemotePalboxRuntime remotePalboxRuntime_;`
- 静态渲染声明：`static void render_remote_palbox(PalworldEditorMod* self);`
- 需要时将 `WorldSessionState` 实例（现有成员，如 `skillSession_` 所在）传给 tick；若现有 world session 成员为 `skill_editor::WorldSessionState` 类型，直接复用。

- [ ] **Step 2: dllmain.cpp 接入**

- `game_thread_tick` 内（`baseResourceBridge_.tick(deltaSeconds)` 附近，见 `dllmain.cpp:117` 起）：
  `remotePalboxRuntime_.tick(deltaSeconds, worldSession_);`（成员名 `worldSession_`，`mod_core.hpp:351`，`skill_editor::WorldSessionState` 类型）
- `begin_world_transition`：`remotePalboxRuntime_.begin_world_transition();`
- `finish_world_transition`：`remotePalboxRuntime_.finish_world_transition();`
- `render_main_window` 分发中新增：
  `PalworldEditorMod::render_remote_palbox(self);`
  （放置位置：现有 PalworldEditor 页签内、`render_base_resource_sharing` 之后即可）
- `render_remote_palbox` 实现放在 `remote_palbox_ui.cpp`，dllmain 只声明/调用。
- 配置路径：构造或 `on_unreal_init` 时计算 ini 路径：
  `GetModuleFileNameW(module 句柄)` 取 DLL 路径（`.../Mods/PalworldEditor/dlls/main.dll`）→ 上级目录 + `remote_palbox.ini`；调用 `remotePalboxRuntime_.load_config(path)`。

- [ ] **Step 3: 构建 + 单测全量**

运行：
```powershell
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor PalworldEditorTests PalworldEditorBaseResourceSharingTests PalworldEditorRemotePalboxTests
ctest --test-dir build --output-on-failure
git diff --check
```
预期：全部通过。

- [ ] **Step 4: 提交**

```bash
git add mods/PalworldEditor/inc/mod/mod_core.hpp mods/PalworldEditor/src/mod/dllmain.cpp
git commit -m "feat(remote-palbox): wire runtime into tick, world transitions, and GUI tab"
```

---

### Task 7: 游戏内验证（手动清单）

**Files:** 无（仅验证）；发现问题则回到对应任务修复并补提交。

- [ ] **Step 1: 部署**

运行：`cmake --build --preset ninja-msvc-x64 --target deploy`

- [ ] **Step 2: 逐项验证（对照 spec 的游戏内验证清单）**

1. 标题界面按 J：无反应、无崩溃、日志无新输出；
2. 进档后按 J：打开原生 Palbox UI，内容与本地终端一致（基地归属正确）；
3. 地牢内按 J：默认禁用（无 UI）；骑乘时按 J：默认禁用；战斗中按 J：默认可开；
4. 打开"仅基地圈内"开关：基地外按 J 禁用、基地内可用；
5. GUI 改键为 K：立即生效；重启游戏后仍是 K（ini 持久化）；
6. 快速连按：不双开；UI 已打开时按键无反应；
7. 无基地的新档：按 J 显示失败消息而非崩溃；
8. 打开/关闭终端全程观察帧时间：无周期性波动（对比开/关远程终端各 1 分钟）；
9. 地牢→传回→LoadMap 场景：按键恢复可用；若发生结构故障，GUI 显示停用原因并在 LoadMap 后恢复；
10. 与资源共享、技能编辑、属性编辑同开：互不干扰。

- [ ] **Step 3: 记录验证结果**

在 PR 描述中附验证结果表；若验证点 1（WBP_PalBox_C 路径）或 BaseCampId/OwnerMapObjectInstanceId 读取失败，按 spec 的 fallback 路径处理并回填设计文档。

---

### Task 8: 收尾

- [ ] **Step 1: 全量验证**

```powershell
cmake --build --preset ninja-msvc-x64 --target format-check PalworldEditor PalworldEditorTests PalworldEditorBaseResourceSharingTests PalworldEditorRemotePalboxTests
ctest --test-dir build --output-on-failure
git diff --check
```

- [ ] **Step 2: 推送并开 PR 到 develop**

```bash
git push -u origin feat/remote-palbox
gh pr create --base develop --head feat/remote-palbox --title "feat: remote palbox with custom hotkey" --body "…"
```
PR body 含：功能说明、4 开关默认值、安全/性能约束落实方式、游戏内验证结果表、验证点回填。

- [ ] **Step 3: 更新设计文档验证点**

将 Task 7 中确认的 WBP_PalBox_C 路径、BaseCampId/OwnerMapObjectInstanceId 读取方式回填到 `docs/superpowers/specs/2026-08-06-remote-palbox-design.md` 的"验证点"节，并入同一 PR。
