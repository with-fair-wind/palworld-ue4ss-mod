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
 * @brief 解析当前世界的可选消歧锚并派生本地玩家控制器。
 * @details 过滤 LoadMap GC 窗口期的旧世界 inventory（RF_PendingKill）；裸
 *          FindFirstOf + GetLocalPalPlayerController 会在旧世界上解析出旧控制器。
 * @return 当前世界的本地玩家控制器；无可用候选时为空，子系统解析仅可接受唯一候选。
 */
[[nodiscard]] auto resolve_world_anchor() -> RC::Unreal::UObject*;

/**
 * @brief 在当前世界的 UPalFishingSystem 上写入覆盖值并记录原值。
 * @param[in] worldContext 可选的世界归属锚（本地玩家控制器）；有值时只接受唯一的
 *            同世界实例，无值时只接受唯一有效实例。候选发现包含 Blueprint 派生类并
 *            排除 CDO/archetype；缺失或歧义时 fail-closed。
 */
[[nodiscard]] auto apply(Ledger& ledger, RC::Unreal::UObject* worldContext) -> GatewayStatus;

/** @brief 按账本恢复仍负责任字段的原值；条件跳过的字段永久退役。 */
[[nodiscard]] auto restore(Ledger& ledger, RC::Unreal::UObject* worldContext) -> GatewayStatus;

}  // namespace fishing_boost
