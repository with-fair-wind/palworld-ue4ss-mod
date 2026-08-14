/**
 * @file capture_override_tests.cpp
 * @brief 捕获覆盖纯值生命周期测试：进程配置、世界切换与安全停用。
 */
#include <iostream>

#include <capture_override/capture_override_state.hpp>

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

void test_disabled_by_default() {
    const capture_override::CaptureOverrideState state;
    CHECK(state.phase() == capture_override::CaptureRuntimePhase::off);
    CHECK(!state.config().enabled);
    CHECK(!state.should_register_hooks());
}

void test_enabled_config_registers_only_in_an_accessible_world() {
    capture_override::CaptureOverrideState state;
    state.set_config({.enabled = true, .forceHundredPercent = true});
    CHECK(!state.should_register_hooks());

    state.begin_world();
    CHECK(state.should_register_hooks());
    state.hooks_registered();
    CHECK(state.phase() == capture_override::CaptureRuntimePhase::hooksRegistered);
    CHECK(!state.should_register_hooks());
}

void test_disabling_requests_hook_removal() {
    capture_override::CaptureOverrideState state;
    state.set_config({.enabled = true});
    state.begin_world();
    state.hooks_registered();

    state.set_config({});
    CHECK(state.should_remove_hooks());
    state.hooks_removed();
    CHECK(state.phase() == capture_override::CaptureRuntimePhase::off);
}

void test_world_transition_preserves_process_config() {
    capture_override::CaptureOverrideState state;
    state.set_config({.enabled = true, .forceHundredPercent = true});
    state.begin_world();
    state.hooks_registered();
    state.hooks_removed();
    state.end_world();

    CHECK(state.config().enabled);
    CHECK(state.config().forceHundredPercent);
    CHECK(!state.should_register_hooks());

    state.begin_world();
    CHECK(state.should_register_hooks());
}

void test_safety_disable_cannot_be_bypassed_in_the_same_world() {
    capture_override::CaptureOverrideState state;
    state.set_config({.enabled = true});
    state.begin_world();
    state.disable_for_world();

    state.set_config({});
    state.set_config({.enabled = true, .forceHundredPercent = true});
    CHECK(state.phase() == capture_override::CaptureRuntimePhase::safetyDisabled);
    CHECK(!state.should_register_hooks());

    state.begin_world();
    CHECK(state.phase() == capture_override::CaptureRuntimePhase::off);
    CHECK(state.should_register_hooks());
}

int main() {
    test_disabled_by_default();
    test_enabled_config_registers_only_in_an_accessible_world();
    test_disabling_requests_hook_removal();
    test_world_transition_preserves_process_config();
    test_safety_disable_cannot_be_bypassed_in_the_same_world();
    if (failures > 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "capture_override tests passed\n";
    return 0;
}
