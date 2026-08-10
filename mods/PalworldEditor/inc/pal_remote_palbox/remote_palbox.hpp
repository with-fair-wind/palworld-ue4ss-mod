/**
 * @file remote_palbox.hpp
 * @brief 远程终端的纯值层：按键上升沿状态机与基地选择策略。
 * @details 本文件只依赖标准库，不接触 Unreal；游戏线程适配见 remote_palbox_runtime。
 */
#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <span>

namespace pal_remote_palbox {

/** @brief 两次触发之间的最短间隔，用于防连点。 */
inline constexpr auto kHotkeyDebounce = std::chrono::milliseconds(300);

/**
 * @brief 按键上升沿状态机：防连点 + 进行中保护。
 * @details 仅在"前帧未按、本帧按下"且距上次触发超过防连点间隔、且不在进行中时触发一次；
 *          长按不重复触发；执行结束（无论成败）由调用方调用 end_trigger() 释放。
 */
class HotkeyEdgeTrigger {
public:
    using clock = std::chrono::steady_clock;
    using time_point = clock::time_point;

    /**
     * @brief 推进状态机。
     * @param[in] now 当前时钟时间。
     * @param[in] isPressed 本帧按键是否按下。
     * @retval true 本帧触发了一次上升沿触发；调用方应在处理结束后调用 end_trigger()。
     * @retval false 未触发。
     */
    [[nodiscard]] auto update(const time_point now, const bool isPressed) -> bool {
        if (inFlight_) {
            return false;
        }
        if (!isPressed) {
            pressed_ = false;
            return false;
        }
        if (pressed_) {
            return false;  // 持续按下，等待下降沿
        }
        pressed_ = true;
        // 首次触发不受防连点限制；之后距上次触发必须超过间隔。
        if (lastTrigger_.has_value() && now - *lastTrigger_ < kHotkeyDebounce) {
            return false;
        }
        lastTrigger_ = now;
        inFlight_ = true;
        return true;
    }

    /** @brief 触发处理结束（无论成败），允许下一次触发。 */
    auto end_trigger() noexcept -> void {
        inFlight_ = false;
    }

    /** @brief 是否正在执行一次触发。 */
    [[nodiscard]] auto in_flight() const noexcept -> bool {
        return inFlight_;
    }

    /** @brief 清空全部状态（LoadMap 或配置变更时使用）。 */
    auto reset() noexcept -> void {
        pressed_ = false;
        inFlight_ = false;
        lastTrigger_ = time_point{};
    }

private:
    bool pressed_{};
    bool inFlight_{};
    std::optional<time_point> lastTrigger_;  // 空表示尚无触发历史；reset() 清空
};

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
