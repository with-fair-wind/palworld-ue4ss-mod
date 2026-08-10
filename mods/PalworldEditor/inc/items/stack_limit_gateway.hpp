/**
 * @file stack_limit_gateway.hpp
 * @brief 声明物品静态数据堆叠上限的 Palworld 游戏线程反射网关。
 */
#pragma once

#include <span>
#include <string>
#include <vector>

#include <items/stack_limit_service.hpp>

/** @brief 提供物品堆叠上限的 Palworld 特定反射适配。 */
namespace item_stack_limit {

/** @brief 一次应用或恢复事务的终止状态。 */
enum class StackLimitGatewayStatus : std::uint8_t {
    succeeded,
    targetUnavailable,
    preflightFailed,
    verificationFailedRolledBack,
    rollbackFailed,
    restorationFailed,
};

/**
 * @brief 把网关应用结果映射为领域账本结果。
 * @param[in] status 网关应用结果。
 * @return 对应的领域账本结果。
 */
[[nodiscard]] constexpr auto to_apply_outcome(const StackLimitGatewayStatus status) noexcept
    -> StackLimitApplyOutcome {
    switch (status) {
        case StackLimitGatewayStatus::succeeded:
            return StackLimitApplyOutcome::succeeded;
        case StackLimitGatewayStatus::targetUnavailable:
            return StackLimitApplyOutcome::targetUnavailable;
        case StackLimitGatewayStatus::preflightFailed:
            return StackLimitApplyOutcome::preflightFailed;
        case StackLimitGatewayStatus::verificationFailedRolledBack:
            return StackLimitApplyOutcome::verifiedRollback;
        case StackLimitGatewayStatus::rollbackFailed:
            return StackLimitApplyOutcome::rollbackFailed;
        case StackLimitGatewayStatus::restorationFailed:
            return StackLimitApplyOutcome::preflightFailed;
    }
    return StackLimitApplyOutcome::preflightFailed;
}

/**
 * @brief 反射事务结果、恢复账本与面向用户的诊断。
 * @details 应用成功时 records 是新增覆盖账本；恢复失败时 records 是剩余恢复责任。
 */
struct StackLimitGatewayResult {
    StackLimitGatewayStatus status{StackLimitGatewayStatus::targetUnavailable};
    std::vector<StackLimitOverrideRecord> records;
    std::string message;

    /** @return 操作是否完成并通过完整重读验证。 */
    [[nodiscard]] auto succeeded() const noexcept -> bool {
        return status == StackLimitGatewayStatus::succeeded;
    }
};

/**
 * @brief 只覆盖原始 `MaxStackCount == 9999` 的普通可堆叠物品。
 * @details 只在当前游戏线程调用内解析、写入和重读，不缓存任何 Unreal 指针。
 * @return 事务状态、成功覆盖记录或回滚失败后的剩余责任。
 */
[[nodiscard]] auto apply_stack_limit_override() -> StackLimitGatewayResult;

/**
 * @brief 按账本恢复仍存在且仍持有本 mod 目标值的对象。
 * @details 恢复时按对象全名重新解析并校验 Raw ID。
 * @param[in] records 本 mod 尚负责恢复的纯值账本。
 * @return 事务状态；失败时 records 只包含仍未解决的恢复责任。
 */
[[nodiscard]] auto restore_stack_limit_override(std::span<const StackLimitOverrideRecord> records)
    -> StackLimitGatewayResult;

}  // namespace item_stack_limit
