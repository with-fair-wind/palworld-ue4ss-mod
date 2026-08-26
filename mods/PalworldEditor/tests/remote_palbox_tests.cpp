/**
 * @file remote_palbox_tests.cpp
 * @brief 远程终端纯值层测试：配置解析、按键状态机、基地选择。
 */
#include <chrono>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <common/hotkey_edge_trigger.hpp>
#include <common/ini_config.hpp>
#include <pal_remote_palbox/remote_palbox.hpp>
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

void test_parse_crlf_and_whitespace() {
    // Windows 编辑器产生的 CRLF 行尾与键值两侧空白都必须被接受。
    const auto config = pal_remote_palbox::parse_remote_palbox_config(
        "HotkeyVk = 75\r\n"
        "DisableWhileMounted = false \r\n"
        "DisableInDungeon=false\r\n"
        "OnlyInsideBaseCircle = true\r\n"
        "DisableDuringCombat = true\r\n");
    CHECK(config.hotkeyVk == 75);
    CHECK(!config.disableWhileMounted);
    CHECK(!config.disableInDungeon);
    CHECK(config.onlyInsideBaseCircle);
    CHECK(config.disableDuringCombat);
}

void test_parse_ini_numeric_blank_and_whitespace_rejected() {
    // 空串与全空白裁剪后为空视图；解析必须短路返回空，而不是对空 data() 做指针算术。
    CHECK(!pal_game::parse_ini_int("").has_value());
    CHECK(!pal_game::parse_ini_int(" \t\r\n").has_value());
    CHECK(!pal_game::parse_ini_float("").has_value());
    CHECK(!pal_game::parse_ini_float("   ").has_value());
    // 正常带空白数值仍应成功，确认短路没有误伤裁剪路径。
    CHECK(pal_game::parse_ini_int(" 75\r\n").value_or(0) == 75);
    CHECK(pal_game::parse_ini_float(" 1.5 ").value_or(0.0F) == 1.5F);
}

void test_edge_trigger_basic() {
    using clock = std::chrono::steady_clock;
    pal_game::HotkeyEdgeTrigger trigger;
    const auto t0 = clock::time_point{};
    CHECK(!trigger.update(t0, false));                            // 未按不触发
    CHECK(!trigger.update(t0 + std::chrono::seconds(1), false));  // 仍未按
    CHECK(trigger.update(t0 + std::chrono::seconds(2), true));    // 上升沿触发
    CHECK(trigger.in_flight());
    CHECK(!trigger.update(t0 + std::chrono::seconds(3), true));  // 进行中忽略
    trigger.end_trigger();
    CHECK(!trigger.in_flight());
}

void test_edge_trigger_debounce_and_repeat() {
    using clock = std::chrono::steady_clock;
    pal_game::HotkeyEdgeTrigger trigger;
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
    pal_game::HotkeyEdgeTrigger trigger;
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
    pal_game::HotkeyEdgeTrigger trigger;
    const auto t0 = clock::time_point{};
    CHECK(trigger.update(t0, true));
    trigger.reset();
    CHECK(!trigger.in_flight());
    CHECK(trigger.update(t0 + std::chrono::seconds(1), true));  // reset 后立即可触发
    trigger.end_trigger();
}

void test_base_camp_selection_prefers_inside() {
    using pal_remote_palbox::BaseCampCandidate;
    using pal_remote_palbox::select_remote_base_camp;
    const std::vector<BaseCampCandidate> camps{
        {.playerInside = false, .distanceSquared = 100.0},
        {.playerInside = false, .distanceSquared = 10.0},
        {.playerInside = true, .distanceSquared = 5000.0},
    };
    const auto pick = select_remote_base_camp(camps);
    CHECK(pick.has_value());
    CHECK(*pick == 2);  // 玩家所在圈优先于更近的圈
}

void test_base_camp_selection_nearest_fallback() {
    using pal_remote_palbox::BaseCampCandidate;
    using pal_remote_palbox::select_remote_base_camp;
    const std::vector<BaseCampCandidate> camps{
        {.playerInside = false, .distanceSquared = 100.0},
        {.playerInside = false, .distanceSquared = 10.0},
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
        {.playerInside = true, .distanceSquared = 1.0},
        {.playerInside = true, .distanceSquared = 0.5},
    };
    const auto pick = select_remote_base_camp(camps);
    CHECK(pick.has_value());
    CHECK(*pick == 0);
}

int main() {
    test_parse_defaults_when_empty();
    test_parse_full_and_roundtrip();
    test_parse_invalid_values_fall_back();
    test_serialize_contains_all_keys();
    test_parse_crlf_and_whitespace();
    test_parse_ini_numeric_blank_and_whitespace_rejected();
    test_edge_trigger_basic();
    test_edge_trigger_debounce_and_repeat();
    test_edge_trigger_held_key_does_not_repeat();
    test_edge_trigger_reset();
    test_base_camp_selection_prefers_inside();
    test_base_camp_selection_nearest_fallback();
    test_base_camp_selection_empty();
    test_base_camp_selection_multiple_inside_takes_first();
    if (failures > 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "remote_palbox tests passed\n";
    return 0;
}
