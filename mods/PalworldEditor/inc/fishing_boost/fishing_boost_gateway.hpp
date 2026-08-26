/**
 * @file fishing_boost_gateway.hpp
 * @brief 声明钓鱼参数覆盖的反射网关。
 */
#pragma once

#include <fishing_boost/fishing_boost_service.hpp>

namespace fishing_boost {

enum class GatewayStatus : std::uint8_t {
    succeeded,
    targetUnavailable,
    preflightFailed,
    verificationFailed,
    rollbackFailed,
};

/** @brief 在 UPalFishingSystem 上写入覆盖值并记录原值。 */
[[nodiscard]] auto apply(Ledger& ledger) -> GatewayStatus;

/** @brief 按账本恢复全部原值。 */
[[nodiscard]] auto restore(Ledger& ledger) -> GatewayStatus;

}  // namespace fishing_boost
