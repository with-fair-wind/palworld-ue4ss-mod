/**
 * @file revive_timer_gateway.hpp
 * @brief 声明终端复活计时移除到 Palworld Unreal 反射的游戏线程网关。
 */
#pragma once

#include <cstdint>
#include <string>

#include <revive_timer/revive_timer_service.hpp>

/** @brief 提供终端复活计时移除的 Palworld 特定反射适配。 */
namespace revive_timer {

/** @brief 一次覆盖或恢复操作的终止状态。 */
enum class ReviveTimerGatewayStatus : std::uint8_t {
    succeeded,         /**< 操作完成并重读验证成功，或无需恢复。 */
    targetUnavailable, /**< 世界上下文或游戏设置实例暂不可用；可重试。 */
    preflightFailed,   /**< 当前游戏版本缺少所需函数或字段，未执行写入。 */
    verifiedRollback,  /**< 写入验证失败，但原值已经恢复并重读确认。 */
    rollbackFailed,    /**< 写入验证失败且原值恢复无法确认。 */
};

/** @brief 把反射网关结果收敛为领域账本的结果分类。 */
[[nodiscard]] constexpr auto to_apply_outcome(const ReviveTimerGatewayStatus status) noexcept
    -> ReviveTimerApplyOutcome {
    switch (status) {
        case ReviveTimerGatewayStatus::succeeded:
            return ReviveTimerApplyOutcome::succeeded;
        case ReviveTimerGatewayStatus::targetUnavailable:
            return ReviveTimerApplyOutcome::targetUnavailable;
        case ReviveTimerGatewayStatus::preflightFailed:
            return ReviveTimerApplyOutcome::preflightFailed;
        case ReviveTimerGatewayStatus::verifiedRollback:
            return ReviveTimerApplyOutcome::verifiedRollback;
        case ReviveTimerGatewayStatus::rollbackFailed:
            return ReviveTimerApplyOutcome::rollbackFailed;
    }
    return ReviveTimerApplyOutcome::preflightFailed;
}

/** @brief 网关应用结果及读取到的原值。 */
struct ReviveTimerApplyResult {
    ReviveTimerGatewayStatus status{ReviveTimerGatewayStatus::targetUnavailable};
    float original{0.0F}; /**< 应用成功或回滚失败时的原值。 */
    std::string message;  /**< 面向用户的诊断。 */
};

/** @brief 网关恢复结果。 */
struct ReviveTimerRestoreResult {
    ReviveTimerGatewayStatus status{ReviveTimerGatewayStatus::targetUnavailable};
    std::string message;
};

/**
 * @brief 解析当前 PalGameSetting 并把 PalBoxReviveTime 写为 0，返回原值。
 * @details 只在游戏线程调用；对象、函数或字段不可用时不执行任何写入。
 */
[[nodiscard]] auto apply_revive_timer_override() -> ReviveTimerApplyResult;

/**
 * @brief 按 Ledger 记录的原值恢复 PalBoxReviveTime。
 * @details 只在游戏线程调用。设置实例已不存在或当前值不再属于本 mod 时视为
 *          无需恢复的成功；写入后重读验证，失败返回 rollbackFailed。
 */
[[nodiscard]] auto restore_revive_timer_override(const float original) -> ReviveTimerRestoreResult;

}  // namespace revive_timer
