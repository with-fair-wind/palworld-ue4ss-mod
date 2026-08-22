/**
 * @file player_state_gate.hpp
 * @brief 玩家状态门控的纯值判定。
 * @details 状态不可读时必须拒绝操作；只有明确读取到 false 才允许继续。
 */
#pragma once

#include <optional>

namespace pal_game {

/**
 * @brief 判断状态门是否允许继续操作。
 * @param[in] state 玩家状态读取结果；空值表示无法安全确认状态。
 * @retval true 明确读取到 false，允许继续。
 * @retval false 状态为 true 或无法读取，必须拦截。
 */
[[nodiscard]] constexpr auto state_gate_allows(const std::optional<bool> state) noexcept -> bool {
    return state.has_value() && !*state;
}

}  // namespace pal_game
