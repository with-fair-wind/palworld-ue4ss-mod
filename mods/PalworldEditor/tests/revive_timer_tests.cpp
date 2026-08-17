/**
 * @file revive_timer_tests.cpp
 * @brief 终端复活计时纯值账本测试：期望、世界代次、恢复责任与安全停用。
 */
#include <iostream>

#include <revive_timer/revive_timer_service.hpp>

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
    const revive_timer::ReviveTimerLedger ledger;
    CHECK(ledger.phase(0) == revive_timer::ReviveTimerRuntimePhase::off);
    CHECK(!ledger.desired());
    CHECK(!ledger.original().has_value());
    CHECK(ledger.next_work(0, true) == revive_timer::ReviveTimerWork::none);
}

void test_apply_only_in_matching_accessible_world() {
    revive_timer::ReviveTimerLedger ledger;
    static_cast<void>(ledger.begin_world(7));
    ledger.set_desired(true);
    CHECK(ledger.next_work(7, false) == revive_timer::ReviveTimerWork::none);
    CHECK(ledger.next_work(8, true) == revive_timer::ReviveTimerWork::none);
    CHECK(ledger.next_work(7, true) == revive_timer::ReviveTimerWork::apply);
    CHECK(ledger.phase(7) == revive_timer::ReviveTimerRuntimePhase::readyToApply);
}

void test_successful_apply_records_original_and_activates() {
    revive_timer::ReviveTimerLedger ledger;
    static_cast<void>(ledger.begin_world(7));
    ledger.set_desired(true);
    CHECK(ledger.begin_apply(7));
    CHECK(ledger.phase(7) == revive_timer::ReviveTimerRuntimePhase::applying);
    CHECK(ledger.next_work(7, true) == revive_timer::ReviveTimerWork::none);

    CHECK(ledger.complete_apply(7, revive_timer::ReviveTimerApplyOutcome::succeeded, 32.5F));
    CHECK(ledger.original().has_value() && *ledger.original() == 32.5F);
    CHECK(ledger.phase(7) == revive_timer::ReviveTimerRuntimePhase::active);
    CHECK(ledger.next_work(7, true) == revive_timer::ReviveTimerWork::none);
}

void test_disabling_requests_restore_then_record_clears() {
    revive_timer::ReviveTimerLedger ledger;
    static_cast<void>(ledger.begin_world(7));
    ledger.set_desired(true);
    static_cast<void>(ledger.begin_apply(7));
    ledger.complete_apply(7, revive_timer::ReviveTimerApplyOutcome::succeeded, 32.5F);

    ledger.set_desired(false);
    CHECK(ledger.phase(7) == revive_timer::ReviveTimerRuntimePhase::restoring);
    CHECK(ledger.next_work(7, true) == revive_timer::ReviveTimerWork::restore);

    CHECK(ledger.complete_restore(true));
    CHECK(!ledger.original().has_value());
    CHECK(ledger.phase(7) == revive_timer::ReviveTimerRuntimePhase::off);
}

void test_target_unavailable_waits_for_retry() {
    revive_timer::ReviveTimerLedger ledger;
    static_cast<void>(ledger.begin_world(7));
    ledger.set_desired(true);
    static_cast<void>(ledger.begin_apply(7));
    CHECK(ledger.complete_apply(7, revive_timer::ReviveTimerApplyOutcome::targetUnavailable, 0.0F));
    CHECK(!ledger.original().has_value());
    CHECK(ledger.phase(7) == revive_timer::ReviveTimerRuntimePhase::waitingForRetry);
    CHECK(ledger.next_work(7, true) == revive_timer::ReviveTimerWork::none);

    ledger.request_retry();
    CHECK(ledger.next_work(7, true) == revive_timer::ReviveTimerWork::apply);
}

void test_preflight_failure_stops_desired_without_safety_disable() {
    revive_timer::ReviveTimerLedger ledger;
    static_cast<void>(ledger.begin_world(7));
    ledger.set_desired(true);
    static_cast<void>(ledger.begin_apply(7));
    CHECK(ledger.complete_apply(7, revive_timer::ReviveTimerApplyOutcome::preflightFailed, 0.0F));
    CHECK(!ledger.desired());
    CHECK(!ledger.original().has_value());
    CHECK(!ledger.safety_disabled());
    CHECK(ledger.phase(7) == revive_timer::ReviveTimerRuntimePhase::off);
}

void test_verified_rollback_clears_responsibility() {
    revive_timer::ReviveTimerLedger ledger;
    static_cast<void>(ledger.begin_world(7));
    ledger.set_desired(true);
    static_cast<void>(ledger.begin_apply(7));
    CHECK(ledger.complete_apply(7, revive_timer::ReviveTimerApplyOutcome::verifiedRollback, 32.5F));
    CHECK(!ledger.original().has_value());
    CHECK(!ledger.desired());
    CHECK(ledger.phase(7) == revive_timer::ReviveTimerRuntimePhase::off);
}

void test_rollback_failure_keeps_responsibility_and_disables() {
    revive_timer::ReviveTimerLedger ledger;
    static_cast<void>(ledger.begin_world(7));
    ledger.set_desired(true);
    static_cast<void>(ledger.begin_apply(7));
    CHECK(ledger.complete_apply(7, revive_timer::ReviveTimerApplyOutcome::rollbackFailed, 32.5F));
    CHECK(ledger.original().has_value() && *ledger.original() == 32.5F);
    CHECK(ledger.safety_disabled());
    CHECK(ledger.next_work(7, true) != revive_timer::ReviveTimerWork::apply);
}

void test_failed_restore_keeps_responsibility() {
    revive_timer::ReviveTimerLedger ledger;
    static_cast<void>(ledger.begin_world(7));
    ledger.set_desired(true);
    static_cast<void>(ledger.begin_apply(7));
    ledger.complete_apply(7, revive_timer::ReviveTimerApplyOutcome::succeeded, 32.5F);
    ledger.set_desired(false);

    CHECK(!ledger.complete_restore(false));
    CHECK(ledger.original().has_value());
    CHECK(ledger.safety_disabled());

    // 世界切换前必须先清偿恢复责任。
    CHECK(!ledger.begin_world(9));
    ledger.allow_restore_retry();
    CHECK(ledger.complete_restore(true));
    CHECK(ledger.begin_world(9));
}

void test_retry_request_is_ignored_with_responsibility_or_safety() {
    revive_timer::ReviveTimerLedger ledger;
    static_cast<void>(ledger.begin_world(7));
    ledger.set_desired(true);
    static_cast<void>(ledger.begin_apply(7));
    ledger.complete_apply(7, revive_timer::ReviveTimerApplyOutcome::rollbackFailed, 32.5F);
    ledger.request_retry();
    CHECK(ledger.next_work(7, true) != revive_timer::ReviveTimerWork::apply);
}

int main() {
    test_disabled_by_default();
    test_apply_only_in_matching_accessible_world();
    test_successful_apply_records_original_and_activates();
    test_disabling_requests_restore_then_record_clears();
    test_target_unavailable_waits_for_retry();
    test_preflight_failure_stops_desired_without_safety_disable();
    test_verified_rollback_clears_responsibility();
    test_rollback_failure_keeps_responsibility_and_disables();
    test_failed_restore_keeps_responsibility();
    test_retry_request_is_ignored_with_responsibility_or_safety();
    if (failures > 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "revive_timer tests passed\n";
    return 0;
}
