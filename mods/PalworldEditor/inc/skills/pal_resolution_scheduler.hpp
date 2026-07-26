/**
 * @file pal_resolution_scheduler.hpp
 * @brief Defines pure scheduling and value state for current-party-Pal resolution.
 */
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include <skills/selected_target_state.hpp>

namespace skill_editor {
/** @brief 后台目标一致性检查的固定最短间隔。 */
inline constexpr auto kTargetValidationInterval = std::chrono::milliseconds{250};

/** @brief 本次 EngineTick 需要解析当前帕鲁的原因。 */
enum class PalResolutionTrigger : std::uint8_t {
    none,
    selectionRequest,
    editRequest,
    validation,
};

/** @brief 在纯值状态上调度立即解析和低频后台校验。 */
class PalResolutionScheduler {
public:
    using clock = std::chrono::steady_clock;
    using time_point = clock::time_point;

    /**
     * @brief 决定当前 tick 是否需要执行 Unreal 目标解析。
     * @details 选择和编辑请求优先于后台校验，并刷新下一次校验截止时间。
     */
    [[nodiscard]] auto decide(const bool validationRequired, const bool selectionRequested,
                              const bool editRequested, const time_point now)
        -> PalResolutionTrigger {
        if (selectionRequested) {
            schedule_next(now);
            return PalResolutionTrigger::selectionRequest;
        }
        if (editRequested) {
            schedule_next(now);
            return PalResolutionTrigger::editRequest;
        }
        if (!validationRequired) {
            nextValidation_.reset();
            return PalResolutionTrigger::none;
        }
        if (!nextValidation_.has_value() || now >= *nextValidation_) {
            schedule_next(now);
            return PalResolutionTrigger::validation;
        }
        return PalResolutionTrigger::none;
    }

    /** @brief 丢弃旧世界或旧目标留下的校验截止时间。 */
    auto reset() noexcept -> void {
        nextValidation_.reset();
    }

private:
    auto schedule_next(const time_point now) -> void {
        nextValidation_ = now + kTargetValidationInterval;
    }

    std::optional<time_point> nextValidation_;
};

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
