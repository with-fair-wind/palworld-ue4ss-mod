/**
 * @file remote_palbox.hpp
 * @brief 远程终端的纯值层：按键上升沿状态机与基地选择策略。
 * @details 本文件只依赖标准库，不接触 Unreal；游戏线程适配见 remote_palbox_runtime。
 */
#pragma once

#include <cstddef>
#include <optional>
#include <span>

#include <common/hotkey_edge_trigger.hpp>

namespace pal_remote_palbox {

/** @brief 按键上升沿状态机已提取为公共原语；别名保持本模块既有引用不变。 */
using HotkeyEdgeTrigger = pal_game::HotkeyEdgeTrigger;

/** @brief 基地选择候选：运行时解析后交给纯值选择器。 */
struct BaseCampCandidate {
    bool playerInside{};      /**< 玩家是否位于该基地圈内。 */
    double distanceSquared{}; /**< 玩家到基地中心的平方距离（兜底排序用）。 */
};

/**
 * @brief 选择远程终端归属基地。
 * @details 策略：优先玩家当前所在圈（第一个 playerInside）；否则取最近基地；无候选返回空。
 */
[[nodiscard]] inline auto select_remote_base_camp(
    const std::span<const BaseCampCandidate> candidates) -> std::optional<std::size_t> {
    for (std::size_t index{}; index < candidates.size(); ++index) {
        if (candidates[index].playerInside) {
            return index;
        }
    }
    std::optional<std::size_t> nearest;
    for (std::size_t index{}; index < candidates.size(); ++index) {
        if (!nearest.has_value() ||
            candidates[index].distanceSquared < candidates[*nearest].distanceSquared) {
            nearest = index;
        }
    }
    return nearest;
}
}  // namespace pal_remote_palbox
