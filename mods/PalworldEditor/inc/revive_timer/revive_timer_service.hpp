/**
 * @file revive_timer_service.hpp
 * @brief 与 Unreal 解耦的终端复活计时移除开关的运行阶段与恢复账本。
 * @details 只有一个持久对象（PalGameSetting 实例）和一个 float 字段，因此账本是
 *          单原值记录；生命周期与错误语义与 StackLimitOverrideLedger 保持一致，
 *          不引入多对象容器。
 */
#pragma once

#include <cstdint>
#include <optional>

/** @brief 提供终端复活计时移除的纯值领域逻辑。 */
namespace revive_timer {

/** @brief 本功能写入的复活等待秒数；0 表示立即复活。 */
inline constexpr float kRemovedReviveSeconds{0.0F};

/** @brief 游戏线程下一次需要执行的一次性工作。 */
enum class ReviveTimerWork : std::uint8_t {
    none,
    apply,
    restore,
};

/** @brief 应用事务完成后交给领域账本的结果分类。 */
enum class ReviveTimerApplyOutcome : std::uint8_t {
    succeeded,
    targetUnavailable,
    preflightFailed,
    verifiedRollback,
    rollbackFailed,
};

/** @brief GUI 与生命周期诊断使用的纯值运行阶段。 */
enum class ReviveTimerRuntimePhase : std::uint8_t {
    off,
    readyToApply,
    applying,
    active,
    waitingForRetry,
    restoring,
    safetyDisabled,
};

/**
 * @brief 管理用户期望、世界代次与单一原值恢复责任。
 * @details 结构或回滚失败后安全停用应用路径；关闭和世界切换仍可继续尝试恢复。
 */
class ReviveTimerLedger final {
public:
    /**
     * @brief 开始新的世界代次。
     * @param[in] worldGeneration 新世界代次。
     * @retval true 没有遗留恢复责任，已切换代次。
     * @retval false 仍有旧世界恢复责任，调用方必须先继续恢复。
     */
    [[nodiscard]] auto begin_world(const std::uint64_t worldGeneration) noexcept -> bool {
        if (original_.has_value()) {
            return false;
        }
        worldGeneration_ = worldGeneration;
        applyInFlight_ = false;
        retryRequired_ = false;
        restoreAttempted_ = false;
        return true;
    }

    /**
     * @brief 更新用户期望；关闭开关不会丢弃恢复责任。
     */
    auto set_desired(const bool enabled) noexcept -> void {
        if (desired_ == enabled) {
            return;
        }
        desired_ = enabled;
        if (!enabled && !original_.has_value()) {
            retryRequired_ = false;
        } else if (enabled && !original_.has_value() && !safetyDisabled_) {
            applyInFlight_ = false;
            retryRequired_ = false;
        }
    }

    /** @return 用户当前是否期望移除复活计时。 */
    [[nodiscard]] auto desired() const noexcept -> bool {
        return desired_;
    }

    /** @return 恢复失败后是否已永久停用本次运行的再次应用。 */
    [[nodiscard]] auto safety_disabled() const noexcept -> bool {
        return safetyDisabled_;
    }

    /**
     * @brief 根据世界状态返回下一项一次性工作。
     */
    [[nodiscard]] auto next_work(const std::uint64_t worldGeneration,
                                 const bool worldAccessible) const noexcept -> ReviveTimerWork {
        if (!worldAccessible || worldGeneration != worldGeneration_) {
            return ReviveTimerWork::none;
        }
        if (!desired_ && original_.has_value() && !restoreAttempted_) {
            return ReviveTimerWork::restore;
        }
        if (desired_ && !original_.has_value() && !applyInFlight_ && !retryRequired_ &&
            !safetyDisabled_) {
            return ReviveTimerWork::apply;
        }
        return ReviveTimerWork::none;
    }

    /**
     * @brief 在调用同步反射网关前占用一次应用工作。
     */
    [[nodiscard]] auto begin_apply(const std::uint64_t worldGeneration) noexcept -> bool {
        if (worldGeneration != worldGeneration_ || !desired_ || applyInFlight_ || retryRequired_ ||
            safetyDisabled_ || original_.has_value()) {
            return false;
        }
        applyInFlight_ = true;
        return true;
    }

    /**
     * @brief 完成一次应用事务并保存原值或未恢复责任。
     * @param[in] original 应用成功时读取到的原值；回滚失败时作为剩余恢复责任。
     */
    [[nodiscard]] auto complete_apply(const std::uint64_t worldGeneration,
                                      const ReviveTimerApplyOutcome outcome,
                                      const float original) noexcept -> bool {
        if (worldGeneration != worldGeneration_ || !applyInFlight_) {
            return false;
        }
        applyInFlight_ = false;

        switch (outcome) {
            case ReviveTimerApplyOutcome::succeeded:
                original_ = original;
                retryRequired_ = false;
                return true;
            case ReviveTimerApplyOutcome::targetUnavailable:
                retryRequired_ = true;
                return true;
            case ReviveTimerApplyOutcome::preflightFailed:
            case ReviveTimerApplyOutcome::verifiedRollback:
                desired_ = false;
                retryRequired_ = false;
                original_.reset();
                return true;
            case ReviveTimerApplyOutcome::rollbackFailed:
                desired_ = false;
                retryRequired_ = false;
                original_ = original;
                safetyDisabled_ = true;
                return true;
        }
        desired_ = false;
        safetyDisabled_ = true;
        return false;
    }

    /** @brief 允许生命周期事件对遗留原值再执行一次恢复，不开放再次应用。 */
    auto allow_restore_retry() noexcept -> void {
        if (original_.has_value()) {
            restoreAttempted_ = false;
        }
    }

    /**
     * @brief 用户显式请求在目标暂不可用后再检测一次。
     * @details 仅当没有恢复责任且未安全停用时清除重试标志，重新开放应用工作。
     */
    auto request_retry() noexcept -> void {
        if (!original_.has_value() && !safetyDisabled_) {
            retryRequired_ = false;
            applyInFlight_ = false;
        }
    }

    /**
     * @brief 完成一次恢复；失败时保留原值责任并安全停用。
     */
    [[nodiscard]] auto complete_restore(const bool succeeded) noexcept -> bool {
        restoreAttempted_ = true;
        if (succeeded) {
            original_.reset();
            restoreAttempted_ = false;
            return true;
        }
        safetyDisabled_ = true;
        return false;
    }

    /**
     * @return 不访问 Unreal 的当前运行阶段。
     */
    [[nodiscard]] auto phase(const std::uint64_t worldGeneration) const noexcept
        -> ReviveTimerRuntimePhase {
        if (worldGeneration != worldGeneration_) {
            return desired_ ? ReviveTimerRuntimePhase::readyToApply : ReviveTimerRuntimePhase::off;
        }
        if (!desired_) {
            if (original_.has_value()) {
                return ReviveTimerRuntimePhase::restoring;
            }
            return safetyDisabled_ ? ReviveTimerRuntimePhase::safetyDisabled
                                   : ReviveTimerRuntimePhase::off;
        }
        if (applyInFlight_) {
            return ReviveTimerRuntimePhase::applying;
        }
        if (original_.has_value()) {
            return ReviveTimerRuntimePhase::active;
        }
        if (retryRequired_) {
            return ReviveTimerRuntimePhase::waitingForRetry;
        }
        return safetyDisabled_ ? ReviveTimerRuntimePhase::safetyDisabled
                               : ReviveTimerRuntimePhase::readyToApply;
    }

    /** @return 仍由本 mod 负责恢复的原值；无责任时为空。 */
    [[nodiscard]] auto original() const noexcept -> std::optional<float> {
        return original_;
    }

private:
    std::optional<float> original_;
    std::uint64_t worldGeneration_{};
    bool desired_{};
    bool applyInFlight_{};
    bool retryRequired_{};
    bool restoreAttempted_{};
    bool safetyDisabled_{};
};

}  // namespace revive_timer
