/**
 * @file unload_cleanup_scheduler.hpp
 * @brief 卸载清理的低频、有界重试纯值状态机。
 */
#pragma once

#include <cstdint>

namespace mod_lifecycle {

/** @brief 卸载清理调度器当前阶段。 */
enum class UnloadCleanupPhase : std::uint8_t {
    idle,           /**< 尚未请求清理。 */
    waitingToRetry, /**< 上次失败，等待固定重试间隔。 */
    inFlight,       /**< 调用方正在执行一次清理。 */
    failed,         /**< 已耗尽尝试次数。 */
    succeeded,      /**< 清理成功或无需清理。 */
};

/** @brief 单次清理尝试的聚合结果；枚举值按严重程度递增，可用 worse_outcome 合并。 */
enum class CleanupOutcome : std::uint8_t {
    succeeded,        /**< 全部必需恢复与 Hook 注销成功。 */
    transientFailure, /**< 存在可重试失败；账本保留恢复责任，重试可能成功。 */
    permanentFailure, /**< 存在不可恢复失败（恢复责任已丢失）；重试无法挽回。 */
};

/** @brief 卸载等待线程的最终判定。 */
enum class UnloadCleanupWaitResult : std::uint8_t {
    cleanupSucceeded, /**< 清理成功或无需清理；可以销毁实例。 */
    cleanupFailed,    /**< 已判定失败（不可恢复损失或重试耗尽）；必须保留实例。 */
    timedOut,         /**< 期限内未得出结论；必须保留实例，避免回调悬垂。 */
};

/**
 * @brief 取两个清理结果中更严重的一个，用于多域结果的合并。
 * @return 严重度更高者（succeeded &lt; transientFailure &lt; permanentFailure）。
 */
[[nodiscard]] constexpr auto worse_outcome(const CleanupOutcome first,
                                           const CleanupOutcome second) noexcept -> CleanupOutcome {
    return second > first ? second : first;
}

/**
 * @brief 限制卸载清理的尝试频率与总次数。
 * @details 首次请求立即执行；失败后每 2 秒重试，总尝试次数（含首次）最多 5 次。成功或耗尽
 *          次数后永久停止，避免卸载失败的旧实例在后续 EngineTick 中持续执行反射。
 * @note 永久失败会立即锁存 destruction_blocked() 释放等待线程，但重试日程照常继续——
 *       保留的实例仍可为账本在案的瞬态失败域恢复游戏状态。
 * @note 重试总预算（首次立即 + 最多 kMaximumAttempts-1 次间隔重试）必须完整落在
 *       dllmain 的卸载等待窗口（kUnloadCleanupTimeout）内，那里有 static_assert 固化；
 *       该预算只含间隔，不含单次清理耗时——清理耗时吃掉余量时退化为等待超时、
 *       保留实例（安全方向），不会误删。
 */
class UnloadCleanupScheduler final {
public:
    /** @brief 包含首次尝试在内的最大清理次数。 */
    static constexpr std::uint8_t kMaximumAttempts{5};
    /** @brief 两次失败清理之间的固定等待秒数。 */
    static constexpr float kRetryIntervalSeconds{2.0F};

    /**
     * @brief 推进倒计时，并在本帧获准执行一次清理时返回 true。
     * @param[in] delta_seconds 自上次 EngineTick 起经过的非负秒数；负值按 0 处理。
     * @retval true 调用方必须执行一次清理并调用 complete()。
     * @retval false 当前仍在等待、已有清理在途或已进入终态。
     */
    [[nodiscard]] auto advance(const float delta_seconds) noexcept -> bool {
        if (phase_ == UnloadCleanupPhase::idle) {
            begin_attempt();
            return true;
        }
        if (phase_ != UnloadCleanupPhase::waitingToRetry) {
            return false;
        }
        if (delta_seconds > 0.0F) {
            remaining_seconds_ =
                delta_seconds >= remaining_seconds_ ? 0.0F : remaining_seconds_ - delta_seconds;
        }
        if (remaining_seconds_ > 0.0F) {
            return false;
        }
        begin_attempt();
        return true;
    }

    /**
     * @brief 完成当前清理；失败时安排下一次有限重试或进入失败终态。
     * @param[in] outcome 本次清理的聚合结果。
     * @note permanentFailure 只锁存销毁阻断，不缩短重试日程；正常情况下永久失败的域
     *       会在后续尝试中继续上报失败，若某个后续尝试整体成功，销毁仍按阻断处理。
     */
    auto complete(const CleanupOutcome outcome) noexcept -> void {
        if (phase_ != UnloadCleanupPhase::inFlight) {
            return;
        }
        if (outcome == CleanupOutcome::succeeded) {
            // 销毁已阻断时即使整体成功也归入 failed 终态：重试已无意义，实例仍不得销毁。
            phase_ =
                destruction_blocked_ ? UnloadCleanupPhase::failed : UnloadCleanupPhase::succeeded;
            remaining_seconds_ = 0.0F;
            return;
        }
        if (outcome == CleanupOutcome::permanentFailure) {
            destruction_blocked_ = true;
        }
        if (attempts_ >= kMaximumAttempts) {
            phase_ = UnloadCleanupPhase::failed;
            remaining_seconds_ = 0.0F;
            return;
        }
        phase_ = UnloadCleanupPhase::waitingToRetry;
        remaining_seconds_ = kRetryIntervalSeconds;
    }

    /** @brief 标记当前实例没有注册 EngineTick，因此无需游戏线程清理。 */
    auto mark_not_required() noexcept -> void {
        if (phase_ == UnloadCleanupPhase::idle) {
            phase_ = UnloadCleanupPhase::succeeded;
        }
    }

    /** @return 当前调度阶段。 */
    [[nodiscard]] auto phase() const noexcept -> UnloadCleanupPhase {
        return phase_;
    }

    /** @return 已开始的清理尝试次数。仅供测试与诊断日志断言重试日程；生产路径不依赖。 */
    [[nodiscard]] auto attempts() const noexcept -> std::uint8_t {
        return attempts_;
    }

    /** @return 是否已出现不可恢复失败；为 true 后实例在任何阶段都不得销毁。 */
    [[nodiscard]] auto destruction_blocked() const noexcept -> bool {
        return destruction_blocked_;
    }

private:
    /** @brief 进入一次在途清理并增加尝试计数。 */
    auto begin_attempt() noexcept -> void {
        ++attempts_;
        remaining_seconds_ = 0.0F;
        phase_ = UnloadCleanupPhase::inFlight;
    }

    UnloadCleanupPhase phase_{UnloadCleanupPhase::idle};
    std::uint8_t attempts_{};
    float remaining_seconds_{};
    bool destruction_blocked_{};
};

}  // namespace mod_lifecycle
