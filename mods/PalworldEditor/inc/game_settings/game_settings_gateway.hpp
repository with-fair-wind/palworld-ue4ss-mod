/**
 * @file game_settings_gateway.hpp
 * @brief 声明 UPalGameSetting 参数覆盖的游戏线程网关。
 */
#pragma once

#include <game_settings/game_settings_override.hpp>

namespace game_settings {

/** @brief 一次覆盖/恢复操作的结果。 */
enum class GatewayStatus : std::uint8_t {
    succeeded,
    targetUnavailable,  /**< GameSetting 实例暂不可用；可重试。 */
    preflightFailed,    /**< 属性不存在或类型不匹配；fail-closed。 */
    verificationFailed, /**< 写后重读不一致（已尝试回滚）。 */
    rollbackFailed,     /**< 回滚后仍不一致；保留恢复责任。 */
};

/** @brief 应用所有待写入的覆盖并记录原值。 */
[[nodiscard]] auto apply_overrides(OverrideLedger& ledger) -> GatewayStatus;

/** @brief 恢复所有有恢复责任的字段到原值。 */
[[nodiscard]] auto restore_overrides(OverrideLedger& ledger) -> GatewayStatus;

}  // namespace game_settings
