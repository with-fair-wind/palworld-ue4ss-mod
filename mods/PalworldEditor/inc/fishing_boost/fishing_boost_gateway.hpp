/**
 * @file fishing_boost_gateway.hpp
 * @brief 声明钓鱼参数覆盖的反射网关。
 */
#pragma once

#include <fishing_boost/fishing_boost_service.hpp>

namespace RC::Unreal {
class UObject;
}

namespace fishing_boost {

enum class GatewayStatus : std::uint8_t {
    succeeded,
    targetUnavailable,
    preflightFailed,
    verificationFailed,
    rollbackFailed,
};

/**
 * @brief 在当前世界的 UPalFishingSystem 上写入覆盖值并记录原值。
 * @param[in] worldContext 当前世界内对象（与远程终端同款的 inventory 派生上下文）；
 *            目标实例按 GetWorld() 与之比对选择，排除 LoadMap GC 窗口期的旧世界实例。
 */
[[nodiscard]] auto apply(Ledger& ledger, RC::Unreal::UObject* worldContext) -> GatewayStatus;

/** @brief 按账本恢复仍负责任字段的原值；条件跳过的字段永久退役不再参与后续重试。 */
[[nodiscard]] auto restore(Ledger& ledger, RC::Unreal::UObject* worldContext) -> GatewayStatus;

}  // namespace fishing_boost
