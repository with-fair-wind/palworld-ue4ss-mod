/**
 * @file pal_revive.hpp
 * @brief 队伍帕鲁复活的游戏线程适配接口与纯值执行结果。
 */
#pragma once

#include <cstdint>

namespace pal_revive {
/** @brief 整次队伍复活在进入逐槽事务前可能发生的结构错误。 */
enum class TeamReviveError : std::uint8_t {
    none,
    holderUnavailable,
    invalidSlotCount,
    handleInterfaceUnavailable,
};

/** @brief 一次队伍复活请求的纯值结果，不持有任何 Unreal 对象。 */
struct TeamReviveResult {
    TeamReviveError error{TeamReviveError::none};
    int revivedCount{};
    int failedCount{};
    bool rollbackFailed{};
};

/**
 * @brief 在当前游戏线程遍历本地队伍并事务性复活非健康帕鲁。
 * @return 结构错误、成功数、失败数和不可恢复回滚状态。
 * @warning 只能在 Unreal 可访问的 EngineTick 游戏线程中调用。
 */
[[nodiscard]] auto revive_team_pals() -> TeamReviveResult;
}  // namespace pal_revive
