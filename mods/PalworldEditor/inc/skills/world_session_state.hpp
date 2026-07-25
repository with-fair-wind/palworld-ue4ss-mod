/**
 * @file world_session_state.hpp
 * @brief Defines pure value state for guarding requests across Unreal world transitions.
 */
#pragma once

#include <cstdint>
#include <optional>

namespace skill_editor
{
/**
 * @brief Tracks the current world generation and whether a Pal target was confirmed in it.
 */
class WorldSessionState
{
public:
    [[nodiscard]] auto generation() const noexcept -> std::uint64_t
    {
        return generation_;
    }

    [[nodiscard]] auto can_access_unreal() const noexcept -> bool
    {
        return !transitioning_;
    }

    [[nodiscard]] auto is_target_confirmed() const noexcept -> bool
    {
        return confirmedGeneration_ == generation_ && can_access_unreal();
    }

    auto begin_transition() noexcept -> void
    {
        ++generation_;
        transitioning_ = true;
        confirmedGeneration_.reset();
    }

    auto finish_transition() noexcept -> void
    {
        transitioning_ = false;
    }

    [[nodiscard]] auto confirm_target() noexcept -> bool
    {
        if (!can_access_unreal())
        {
            return false;
        }

        confirmedGeneration_ = generation_;
        return true;
    }

    [[nodiscard]] auto request_targets_current_world(
        const std::uint64_t requestGeneration) const noexcept -> bool
    {
        return can_access_unreal() && requestGeneration == generation_;
    }

    [[nodiscard]] auto request_is_current(
        const std::uint64_t requestGeneration) const noexcept -> bool
    {
        return request_targets_current_world(requestGeneration) && is_target_confirmed();
    }

private:
    std::uint64_t generation_{};
    bool transitioning_{};
    std::optional<std::uint64_t> confirmedGeneration_;
};
}  // namespace skill_editor
