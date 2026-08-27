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
    /**< apply：写入并验证完成；restore：恢复完成或确认目标不存在（责任解除）。 */
    targetUnavailable,
    /**< 世界锚或目标子系统无法解析（unknown）：瞬态，账本保持，按有界重试再次尝试。 */
    preflightFailed,
    verificationFailed,
    rollbackFailed,
};

/**
 * @brief 解析当前世界的锚并派生本地玩家控制器。
 * @details 过滤 LoadMap GC 窗口期的旧世界 inventory（RF_PendingKill）；裸
 *          FindFirstOf + GetLocalPalPlayerController 会在旧世界上解析出旧控制器。
 * @return 当前世界的本地玩家控制器；无可用候选（含重试窗口期）时为空。
 */
[[nodiscard]] auto resolve_world_anchor() -> RC::Unreal::UObject*;

/**
 * @brief 在当前世界的 UPalFishingSystem 上写入覆盖值并记录原值。
 * @param[in] worldContext 带世界归属的锚对象（本地玩家控制器）；目标实例按
 *            GetWorld() 与之比对选择，排除 LoadMap GC 窗口期的旧世界实例。
 */
[[nodiscard]] auto apply(Ledger& ledger, RC::Unreal::UObject* worldContext) -> GatewayStatus;

/** @brief 按账本恢复仍负责任字段的原值；条件跳过的字段永久退役，锚无效时保留账本瞬态重试。 */
[[nodiscard]] auto restore(Ledger& ledger, RC::Unreal::UObject* worldContext) -> GatewayStatus;

}  // namespace fishing_boost
