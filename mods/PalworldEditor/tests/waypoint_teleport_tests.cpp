/**
 * @file waypoint_teleport_tests.cpp
 * @brief 标记点传送纯值测试：最近标记选择、配置解析与公共按键状态机。
 */
#include <chrono>
#include <iostream>
#include <span>

#include <common/hotkey_edge_trigger.hpp>
#include <waypoint_teleport/waypoint_teleport_domain.hpp>

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

void test_nearest_marker_selection() {
    const waypoint_teleport::MarkerCandidate candidates[]{
        {.x = 100.0, .y = 0.0, .z = 10.0, .distanceSquared = 100.0},
        {.x = 0.0, .y = 5.0, .z = 20.0, .distanceSquared = 25.0},
        {.x = 50.0, .y = 50.0, .z = 30.0, .distanceSquared = 5000.0},
    };
    const auto nearest = waypoint_teleport::select_nearest_marker(candidates);
    CHECK(nearest.has_value());
    CHECK(*nearest == 1);
}

void test_nearest_marker_empty() {
    constexpr std::span<const waypoint_teleport::MarkerCandidate> empty{};
    CHECK(!waypoint_teleport::select_nearest_marker(empty).has_value());
    CHECK(!waypoint_teleport::select_nearest_marker({}).has_value());
}

void test_nearest_marker_single() {
    const waypoint_teleport::MarkerCandidate candidates[]{
        {.x = 1.0, .y = 2.0, .z = 3.0, .distanceSquared = 9.9}};
    const auto nearest = waypoint_teleport::select_nearest_marker(candidates);
    CHECK(nearest.has_value() && *nearest == 0);
}

void test_config_defaults_and_round_trip() {
    const auto config = waypoint_teleport::kDefaultWaypointTeleportConfig;
    CHECK(config.hotkeyVk == 118);
    CHECK(config.arrivalHeightOffset == 10000.0F);

    const auto parsed = waypoint_teleport::parse_waypoint_teleport_config(
        waypoint_teleport::serialize_waypoint_teleport_config(config));
    CHECK(parsed.hotkeyVk == config.hotkeyVk);
    CHECK(parsed.disableWhileMounted == config.disableWhileMounted);
    CHECK(parsed.disableInDungeon == config.disableInDungeon);
    CHECK(parsed.disableDuringCombat == config.disableDuringCombat);
    CHECK(parsed.arrivalHeightOffset == config.arrivalHeightOffset);
}

void test_config_invalid_values_fall_back() {
    const auto parsed = waypoint_teleport::parse_waypoint_teleport_config(
        "HotkeyVk=0\n"
        "ArrivalHeightOffset=1e9\n"
        "DisableWhileMounted=banana\n");
    CHECK(parsed.hotkeyVk == waypoint_teleport::kDefaultWaypointTeleportConfig.hotkeyVk);
    CHECK(parsed.arrivalHeightOffset ==
          waypoint_teleport::kDefaultWaypointTeleportConfig.arrivalHeightOffset);
    CHECK(parsed.disableWhileMounted ==
          waypoint_teleport::kDefaultWaypointTeleportConfig.disableWhileMounted);
}

void test_config_unknown_keys_ignored() {
    const auto parsed =
        waypoint_teleport::parse_waypoint_teleport_config("HotkeyVk=66\nUnknown=x\n");
    CHECK(parsed.hotkeyVk == 66);
}

void test_hotkey_edge_trigger_reuse() {
    // 公共原语自远程终端提取；回归其在重复触发、防连点与重置下的核心行为。
    pal_game::HotkeyEdgeTrigger trigger;
    const auto now = std::chrono::steady_clock::now();
    CHECK(trigger.update(now, true));
    trigger.end_trigger();
    // 长按不重复触发。
    CHECK(!trigger.update(now + std::chrono::milliseconds{1}, true));
    // 下降沿后进入防连点窗口。
    CHECK(!trigger.update(now + std::chrono::milliseconds{2}, false));
    CHECK(!trigger.update(now + std::chrono::milliseconds{3}, true));
    // 超过防连点间隔后允许再次触发：先松开清空按下状态（返回 false），再按新键。
    const auto later = now + pal_game::kHotkeyDebounce + std::chrono::milliseconds{10};
    CHECK(!trigger.update(later, false));
    CHECK(trigger.update(later + std::chrono::milliseconds{1}, true));
    // 进行中保护：end_trigger 前不触发。
    CHECK(!trigger.update(later + std::chrono::milliseconds{500}, false));
    CHECK(!trigger.update(later + std::chrono::milliseconds{501}, true));
    trigger.end_trigger();
    trigger.reset();
    CHECK(!trigger.in_flight());
}

int main() {
    test_nearest_marker_selection();
    test_nearest_marker_empty();
    test_nearest_marker_single();
    test_config_defaults_and_round_trip();
    test_config_invalid_values_fall_back();
    test_config_unknown_keys_ignored();
    test_hotkey_edge_trigger_reuse();
    if (failures > 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "waypoint_teleport tests passed\n";
    return 0;
}
