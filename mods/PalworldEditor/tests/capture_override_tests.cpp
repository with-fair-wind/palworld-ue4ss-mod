/**
 * @file capture_override_tests.cpp
 * @brief 捕获覆盖纯值生命周期测试：进程配置、双开关独立性、世界切换与安全停用。
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
    CHECK(!state.config().unlockUncapturable);
    CHECK(!state.config().any_active());
    CHECK(!state.should_register_hooks());
}

void test_enabled_config_registers_only_in_an_accessible_world() {
    capture_override::CaptureOverrideState state;
    state.set_config({.unlockUncapturable = true, .forceHundredPercent = true});
    CHECK(!state.should_register_hooks());

    state.begin_world();
    CHECK(state.should_register_hooks());
    state.hooks_registered();
    CHECK(state.phase() == capture_override::CaptureRuntimePhase::hooksRegistered);
    CHECK(!state.should_register_hooks());
}

void test_disabling_requests_hook_removal() {
    capture_override::CaptureOverrideState state;
    state.set_config({.unlockUncapturable = true});
    state.begin_world();
    state.hooks_registered();

    state.set_config({});
    CHECK(state.should_remove_hooks());
    state.hooks_removed();
    CHECK(state.phase() == capture_override::CaptureRuntimePhase::off);
}

void test_force_only_registers_hooks_without_unlock() {
    capture_override::CaptureOverrideState state;
    state.set_config({.forceHundredPercent = true});
    state.begin_world();
    CHECK(state.config().any_active());
    CHECK(state.should_register_hooks());
    state.hooks_registered();

    // 解锁与强制相互独立：仅关闭解锁不应注销 Hook。
    state.set_config({.forceHundredPercent = true});
    CHECK(!state.should_remove_hooks());

    // 两者皆关才请求注销。
    state.set_config({});
    CHECK(state.should_remove_hooks());
}

void test_unlock_only_keeps_hooks_when_force_toggles() {
    capture_override::CaptureOverrideState state;
    state.set_config({.unlockUncapturable = true});
    state.begin_world();
    state.hooks_registered();

    state.set_config({.unlockUncapturable = true, .forceHundredPercent = true});
    CHECK(!state.should_remove_hooks());
    state.set_config({.unlockUncapturable = true});
    CHECK(!state.should_remove_hooks());
}

void test_world_transition_preserves_process_config() {
    capture_override::CaptureOverrideState state;
    state.set_config({.unlockUncapturable = true, .forceHundredPercent = true});
    state.begin_world();
    state.hooks_registered();
    state.hooks_removed();
    state.end_world();

    CHECK(state.config().unlockUncapturable);
    CHECK(state.config().forceHundredPercent);
    CHECK(!state.should_register_hooks());

    state.begin_world();
    CHECK(state.should_register_hooks());
}

void test_safety_disable_cannot_be_bypassed_in_the_same_world() {
    capture_override::CaptureOverrideState state;
    state.set_config({.unlockUncapturable = true});
    state.begin_world();
    state.disable_for_world();

    state.set_config({});
    state.set_config({.unlockUncapturable = true, .forceHundredPercent = true});
    CHECK(state.phase() == capture_override::CaptureRuntimePhase::safetyDisabled);
    CHECK(!state.should_register_hooks());

    state.begin_world();
    CHECK(state.phase() == capture_override::CaptureRuntimePhase::off);
    CHECK(state.should_register_hooks());
}

void test_unavailable_target_skips_without_disabling_the_world() {
    capture_override::CaptureOverrideState state;
    state.set_config({.unlockUncapturable = true});
    state.begin_world();
    state.hooks_registered();

    state.observe_preparation_status(capture_override::CapturePreparationStatus::unavailable);
    CHECK(state.phase() == capture_override::CaptureRuntimePhase::hooksRegistered);

    state.observe_preparation_status(capture_override::CapturePreparationStatus::incompatible);
    CHECK(state.phase() == capture_override::CaptureRuntimePhase::safetyDisabled);
}

int main() {
    test_disabled_by_default();
    test_enabled_config_registers_only_in_an_accessible_world();
    test_disabling_requests_hook_removal();
    test_force_only_registers_hooks_without_unlock();
    test_unlock_only_keeps_hooks_when_force_toggles();
    test_world_transition_preserves_process_config();
    test_safety_disable_cannot_be_bypassed_in_the_same_world();
    test_unavailable_target_skips_without_disabling_the_world();
    if (failures > 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "capture_override tests passed\n";
    return 0;
}
