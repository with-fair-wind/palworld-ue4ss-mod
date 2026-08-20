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

/**
 * @brief 限制卸载清理的尝试频率与总次数。
 * @details 首次请求立即执行；失败后每 2 秒重试，最多 5 次。成功或耗尽次数后永久停止，
 *          避免卸载失败的旧实例在后续 EngineTick 中持续执行反射。
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
     * @param[in] succeeded 全部必需恢复和 Hook 注销是否成功。
     */
    auto complete(const bool succeeded) noexcept -> void {
        if (phase_ != UnloadCleanupPhase::inFlight) {
            return;
        }
        if (succeeded) {
            phase_ = UnloadCleanupPhase::succeeded;
            remaining_seconds_ = 0.0F;
            return;
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

    /** @return 已开始的清理尝试次数。 */
    [[nodiscard]] auto attempts() const noexcept -> std::uint8_t {
        return attempts_;
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
};

}  // namespace mod_lifecycle
