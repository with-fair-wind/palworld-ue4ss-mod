/**
 * @file unload_cleanup_scheduler_tests.cpp
 * @brief 卸载清理调度器的纯值测试：有界重试、终态与永久失败销毁阻断。
 */
#include <iostream>

#include <mod/unload_cleanup_scheduler.hpp>

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

void test_transient_failure_retries_at_a_bounded_rate() {
    using mod_lifecycle::CleanupOutcome;
    using mod_lifecycle::UnloadCleanupPhase;
    using mod_lifecycle::UnloadCleanupScheduler;

    UnloadCleanupScheduler scheduler;
    CHECK(scheduler.advance(0.0F));
    CHECK(scheduler.attempts() == 1);
    CHECK(scheduler.phase() == UnloadCleanupPhase::inFlight);
    CHECK(!scheduler.advance(UnloadCleanupScheduler::kRetryIntervalSeconds));

    while (scheduler.attempts() < UnloadCleanupScheduler::kMaximumAttempts) {
        scheduler.complete(CleanupOutcome::transientFailure);
        CHECK(scheduler.phase() == UnloadCleanupPhase::waitingToRetry);
        CHECK(!scheduler.destruction_blocked());
        CHECK(!scheduler.advance(-1.0F));
        CHECK(!scheduler.advance(UnloadCleanupScheduler::kRetryIntervalSeconds - 0.25F));
        CHECK(scheduler.advance(0.25F));
    }

    scheduler.complete(CleanupOutcome::transientFailure);
    CHECK(scheduler.phase() == UnloadCleanupPhase::failed);
    CHECK(scheduler.attempts() == UnloadCleanupScheduler::kMaximumAttempts);
    CHECK(!scheduler.destruction_blocked());
    CHECK(!scheduler.advance(60.0F));
}

void test_success_stops_retrying() {
    using mod_lifecycle::CleanupOutcome;
    using mod_lifecycle::UnloadCleanupPhase;
    using mod_lifecycle::UnloadCleanupScheduler;

    UnloadCleanupScheduler scheduler;
    CHECK(scheduler.advance(0.0F));
    scheduler.complete(CleanupOutcome::transientFailure);
    CHECK(scheduler.phase() == UnloadCleanupPhase::waitingToRetry);
    CHECK(scheduler.advance(UnloadCleanupScheduler::kRetryIntervalSeconds));
    scheduler.complete(CleanupOutcome::succeeded);
    CHECK(scheduler.phase() == UnloadCleanupPhase::succeeded);
    CHECK(scheduler.attempts() == 2);
    CHECK(!scheduler.destruction_blocked());
    CHECK(!scheduler.advance(60.0F));
}

void test_permanent_failure_blocks_destruction_but_keeps_retrying() {
    using mod_lifecycle::CleanupOutcome;
    using mod_lifecycle::UnloadCleanupPhase;
    using mod_lifecycle::UnloadCleanupScheduler;

    UnloadCleanupScheduler scheduler;
    CHECK(scheduler.advance(0.0F));
    scheduler.complete(CleanupOutcome::permanentFailure);
    CHECK(scheduler.destruction_blocked());
    CHECK(scheduler.phase() == UnloadCleanupPhase::waitingToRetry);
    // 永久失败后重试日程继续：保留的实例仍可为瞬态失败域恢复游戏状态。
    CHECK(scheduler.advance(UnloadCleanupScheduler::kRetryIntervalSeconds));
    CHECK(scheduler.attempts() == 2);

    while (scheduler.attempts() < UnloadCleanupScheduler::kMaximumAttempts) {
        scheduler.complete(CleanupOutcome::transientFailure);
        CHECK(scheduler.phase() == UnloadCleanupPhase::waitingToRetry);
        CHECK(scheduler.advance(UnloadCleanupScheduler::kRetryIntervalSeconds));
    }
    scheduler.complete(CleanupOutcome::permanentFailure);
    CHECK(scheduler.phase() == UnloadCleanupPhase::failed);
    CHECK(scheduler.attempts() == UnloadCleanupScheduler::kMaximumAttempts);
    CHECK(scheduler.destruction_blocked());
    CHECK(!scheduler.advance(60.0F));
}

void test_permanent_failure_is_never_forgotten() {
    using mod_lifecycle::CleanupOutcome;
    using mod_lifecycle::UnloadCleanupPhase;
    using mod_lifecycle::UnloadCleanupScheduler;

    UnloadCleanupScheduler scheduler;
    CHECK(scheduler.advance(0.0F));
    scheduler.complete(CleanupOutcome::permanentFailure);
    CHECK(scheduler.advance(UnloadCleanupScheduler::kRetryIntervalSeconds));
    scheduler.complete(CleanupOutcome::succeeded);
    // 销毁阻断不被后续成功洗白：调度进入 failed 终态，等待线程按判定失败处理。
    CHECK(scheduler.phase() == UnloadCleanupPhase::failed);
    CHECK(scheduler.destruction_blocked());
    CHECK(scheduler.attempts() == 2);
    CHECK(!scheduler.advance(60.0F));
}

void test_no_work_marks_succeeded() {
    using mod_lifecycle::CleanupOutcome;
    using mod_lifecycle::UnloadCleanupPhase;
    using mod_lifecycle::UnloadCleanupScheduler;

    UnloadCleanupScheduler no_work;
    no_work.mark_not_required();
    CHECK(no_work.phase() == UnloadCleanupPhase::succeeded);
    CHECK(no_work.attempts() == 0);
    CHECK(!no_work.advance(60.0F));
    CHECK(!no_work.destruction_blocked());

    // mark_not_required 只在 idle 阶段生效，不会覆盖已在进行的重试日程。
    UnloadCleanupScheduler retrying;
    CHECK(retrying.advance(0.0F));
    retrying.complete(CleanupOutcome::transientFailure);
    retrying.mark_not_required();
    CHECK(retrying.phase() == UnloadCleanupPhase::waitingToRetry);
}

void test_worse_outcome_orders_by_severity() {
    using mod_lifecycle::CleanupOutcome;
    using mod_lifecycle::worse_outcome;
    CHECK(worse_outcome(CleanupOutcome::succeeded, CleanupOutcome::succeeded) ==
          CleanupOutcome::succeeded);
    CHECK(worse_outcome(CleanupOutcome::succeeded, CleanupOutcome::transientFailure) ==
          CleanupOutcome::transientFailure);
    CHECK(worse_outcome(CleanupOutcome::transientFailure, CleanupOutcome::succeeded) ==
          CleanupOutcome::transientFailure);
    CHECK(worse_outcome(CleanupOutcome::permanentFailure, CleanupOutcome::transientFailure) ==
          CleanupOutcome::permanentFailure);
    CHECK(worse_outcome(CleanupOutcome::transientFailure, CleanupOutcome::permanentFailure) ==
          CleanupOutcome::permanentFailure);
}

int main() {
    test_transient_failure_retries_at_a_bounded_rate();
    test_success_stops_retrying();
    test_permanent_failure_blocks_destruction_but_keeps_retrying();
    test_permanent_failure_is_never_forgotten();
    test_no_work_marks_succeeded();
    test_worse_outcome_orders_by_severity();
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    return 0;
}
