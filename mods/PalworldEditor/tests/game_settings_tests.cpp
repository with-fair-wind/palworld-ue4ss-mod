/**
 * @file game_settings_tests.cpp
 * @brief 游戏参数覆盖纯值账本测试：差量 pending、首次原值保留与恢复责任。
 */
#include <iostream>

#include <game_settings/game_settings_override.hpp>

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

void test_default_state_is_idle() {
    const game_settings::OverrideLedger ledger;
    CHECK(ledger.pending_indices().empty());
    CHECK(ledger.restoring_indices().empty());
    CHECK(!ledger.has_work());
    CHECK(ledger.phase() == game_settings::RuntimePhase::off);
    const auto summary = ledger.status_summary();
    CHECK(summary.desiredCount == 0 && summary.appliedCount == 0 && summary.restoringCount == 0);
}

void test_first_apply_flows_through_pending_to_active() {
    game_settings::OverrideLedger ledger;
    ledger.set_desired(0, std::int32_t{7});
    CHECK(ledger.pending_indices().size() == 1);
    CHECK(ledger.has_work());
    ledger.record_applied(0, std::int32_t{5}, std::int32_t{7});
    CHECK(ledger.pending_indices().empty());
    CHECK(ledger.phase() == game_settings::RuntimePhase::active);
    const auto summary = ledger.status_summary();
    CHECK(summary.desiredCount == 1 && summary.appliedCount == 1 && summary.restoringCount == 0);
}

void test_value_edit_after_apply_reenters_pending_and_keeps_first_original() {
    game_settings::OverrideLedger ledger;
    ledger.set_desired(0, std::int32_t{7});
    ledger.record_applied(0, std::int32_t{5}, std::int32_t{7});

    // 修改后的值必须重新写入（旧实现只认 originals，导致改值永远不生效）。
    ledger.set_desired(0, std::int32_t{8});
    const auto pending = ledger.pending_indices();
    CHECK(pending.size() == 1 && pending[0] == 0);

    // 重写后的原值快照仍是首次写入前读到的 5，恢复回到游戏原生值。
    ledger.record_applied(0, std::int32_t{7}, std::int32_t{8});
    CHECK(ledger.pending_indices().empty());
    CHECK(ledger.original(0).has_value() && std::get<std::int32_t>(*ledger.original(0)) == 5);
}

void test_same_value_edit_does_not_reapply() {
    game_settings::OverrideLedger ledger;
    ledger.set_desired(9, 5000.0F);
    ledger.record_applied(9, 3000.0F, 5000.0F);
    ledger.set_desired(9, 5000.0F);
    CHECK(ledger.pending_indices().empty());
    CHECK(!ledger.has_work());
}

void test_clear_desired_requests_restore_then_record_clears() {
    game_settings::OverrideLedger ledger;
    ledger.set_desired(0, std::int32_t{5});
    ledger.record_applied(0, std::int32_t{5}, std::int32_t{5});
    ledger.clear_desired(0);
    CHECK(ledger.restoring_indices().size() == 1);
    CHECK(ledger.has_work());
    const auto summary = ledger.status_summary();
    CHECK(summary.restoringCount == 1 && summary.desiredCount == 0);
    ledger.clear_record(0);
    CHECK(ledger.restoring_indices().empty());
    CHECK(ledger.pending_indices().empty());
    CHECK(ledger.phase() == game_settings::RuntimePhase::off);
}

void test_unsafe_and_mismatched_values_are_rejected() {
    game_settings::OverrideLedger ledger;
    // 超出安全域。
    ledger.set_desired(0, std::int32_t{99});
    CHECK(!ledger.desired(0).has_value());
    // 类型不匹配（int 槽位写 float）。
    ledger.set_desired(0, 5.0F);
    CHECK(!ledger.desired(0).has_value());
    CHECK(ledger.pending_indices().empty());
}

void test_begin_world_clears_records_and_safety_but_keeps_desired() {
    game_settings::OverrideLedger ledger;
    ledger.set_desired(0, std::int32_t{7});
    ledger.record_applied(0, std::int32_t{5}, std::int32_t{7});
    ledger.disable_for_world();
    CHECK(ledger.phase() == game_settings::RuntimePhase::safetyDisabled);

    ledger.begin_world();
    CHECK(!ledger.safety_disabled());
    CHECK(!ledger.original(0).has_value());
    CHECK(ledger.phase() == game_settings::RuntimePhase::off);
    // 期望保留：调用方决定切图时是否清空（dllmain 会先 clear_all_desired）。
    CHECK(ledger.desired(0).has_value());
    CHECK(ledger.pending_indices().size() == 1);
}

void test_clear_all_desired_makes_every_record_restoring() {
    game_settings::OverrideLedger ledger;
    ledger.set_desired(0, std::int32_t{7});
    ledger.record_applied(0, std::int32_t{5}, std::int32_t{7});
    ledger.set_desired(9, 5000.0F);
    ledger.record_applied(9, 3000.0F, 5000.0F);
    ledger.clear_all_desired();
    CHECK(ledger.restoring_indices().size() == 2);
    ledger.clear_all_records();
    CHECK(ledger.restoring_indices().empty());
    CHECK(ledger.pending_indices().empty());
}

int main() {
    test_default_state_is_idle();
    test_first_apply_flows_through_pending_to_active();
    test_value_edit_after_apply_reenters_pending_and_keeps_first_original();
    test_same_value_edit_does_not_reapply();
    test_clear_desired_requests_restore_then_record_clears();
    test_unsafe_and_mismatched_values_are_rejected();
    test_begin_world_clears_records_and_safety_but_keeps_desired();
    test_clear_all_desired_makes_every_record_restoring();
    if (failures > 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "game_settings tests passed\n";
    return 0;
}
