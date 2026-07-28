/**
 * @file cooldown_gateway.hpp
 * @brief 声明爪钩枪冷却覆盖到 Palworld Unreal 反射的游戏线程网关。
 */
#pragma once

#include <span>
#include <string>
#include <vector>

#include <grappling_hook/cooldown_service.hpp>

/** @brief 提供爪钩枪冷却的 Palworld 特定反射适配。 */
namespace grappling_hook {

/** @brief 一次覆盖或恢复操作的终止状态。 */
enum class CooldownGatewayStatus {
    succeeded,          /**< 操作完成并重读验证成功。 */
    targetUnavailable,  /**< 当前没有可明确识别的正式爪钩枪对象。 */
    layoutUnavailable,  /**< 当前游戏版本缺少所需字段，未执行写入。 */
    verificationFailed, /**< 写入后的属性值与期望不一致。 */
};

/** @brief 网关操作结果及新建的原值恢复记录。 */
struct CooldownGatewayResult {
    CooldownGatewayStatus status{CooldownGatewayStatus::targetUnavailable}; /**< 终止状态。 */
    std::vector<CooldownOverrideRecord> records; /**< 应用成功时的原值账本。 */
    std::string message;                         /**< 面向用户的诊断。 */

    /** @return 操作是否完成并通过验证。 */
    [[nodiscard]] auto succeeded() const noexcept -> bool {
        return status == CooldownGatewayStatus::succeeded;
    }
};

/**
 * @brief 仅在游戏线程扫描和读写当前正式爪钩枪对象。
 * @details 不缓存 Unreal 指针；应用时按 `ownItemID.StaticId` 精确识别，恢复时按对象全名重新解析。
 */
class GrappleCooldownGateway final {
public:
    /** @brief 扫描一次当前武器对象，覆盖正式爪钩枪并返回各自原值。 */
    [[nodiscard]] auto apply() -> CooldownGatewayResult;

    /** @brief 按账本恢复仍存在且仍持有本 mod 覆盖值的对象。 */
    [[nodiscard]] auto restore(std::span<const CooldownOverrideRecord> records)
        -> CooldownGatewayResult;
};

}  // namespace grappling_hook
