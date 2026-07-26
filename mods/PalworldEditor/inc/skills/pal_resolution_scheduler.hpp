/**
 * @file pal_resolution_scheduler.hpp
 * @brief Defines pure scheduling and value state for current-party-Pal resolution.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include <skills/selected_target_state.hpp>

namespace skill_editor {
/** @brief 本次 EngineTick 需要解析当前帕鲁的原因。 */
enum class PalResolutionTrigger : std::uint8_t {
    none,
    selectionRequest,
    editRequest,
};

/**
 * @brief 根据显式 GUI 事件决定当前 EngineTick 是否需要解析当前帕鲁。
 * @details 选择优先于编辑；没有请求时始终不执行后台解析。
 */
[[nodiscard]] constexpr auto decide_pal_resolution(const bool selectionRequested,
                                                   const bool editRequested) noexcept
    -> PalResolutionTrigger {
    if (selectionRequested) {
        return PalResolutionTrigger::selectionRequest;
    }
    if (editRequested) {
        return PalResolutionTrigger::editRequest;
    }
    return PalResolutionTrigger::none;
}

/** @brief 不含 Unreal 指针的当前帕鲁解析结果。 */
struct TargetResolutionSnapshot {
    bool resolved{};
    SelectedTargetObservation observation;
    SelectedTargetResolutionStatus status{
        SelectedTargetResolutionStatus::holderCandidatesUnavailable};
    std::size_t holderCandidateCount{};
    std::size_t localHolderCandidateCount{};
    std::wstring holderCandidateClasses;

    auto operator==(const TargetResolutionSnapshot&) const -> bool = default;
};

/** @brief 保存最近一次纯值解析结果并报告可观察变化。 */
class TargetResolutionState {
public:
    [[nodiscard]] auto update(TargetResolutionSnapshot next) -> bool {
        if (current_ == next) {
            return false;
        }
        current_ = std::move(next);
        return true;
    }

    auto reset() -> void {
        current_ = {};
    }

    [[nodiscard]] auto current() const -> const TargetResolutionSnapshot& {
        return current_;
    }

private:
    TargetResolutionSnapshot current_;
};
}  // namespace skill_editor
