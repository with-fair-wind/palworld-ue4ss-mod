/**
 * @file hotkey_edge_trigger.hpp
 * @brief 按键上升沿状态机的公共纯值原语：防连点 + 进行中保护。
 * @details 由远程终端与标记点传送共用；只依赖标准库，不接触 Unreal。
 */
#pragma once

#include <chrono>
#include <optional>

namespace pal_game {

/** @brief 两次触发之间的最短间隔，用于防连点。 */
inline constexpr auto kHotkeyDebounce = std::chrono::milliseconds{300};

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

}  // namespace pal_game
