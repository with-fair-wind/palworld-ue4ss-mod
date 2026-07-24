/**
 * @file selected_target_state.hpp
 * @brief 定义当前待出战帕鲁目标的纯值状态和请求目标校验。
 */
#pragma once

#include <skills/skill_editor_service.hpp>

#include <functional>
#include <optional>
#include <string>
#include <utility>

/** @brief 提供当前待出战帕鲁目标的纯 C++ 状态管理。 */
namespace skill_editor {

/**
 * @brief 当前待出战帕鲁的一次观测结果。
 */
struct SelectedTargetObservation {
    /** @brief 当前帕鲁参数对象对应的不透明目标句柄；0 表示没有可用目标。 */
    SkillTarget target{};

    /** @brief 用于界面展示的角色 ID 或名称。 */
    std::string name;
};

/**
 * @brief 保存当前待出战帕鲁，并判断目标身份是否发生变化。
 */
class SelectedTargetState {
public:
    /**
     * @brief 用最新观测替换当前状态。
     * @param[in] observation 当前帧观测结果。
     * @return 目标句柄发生变化时返回 `true`；仅名称变化时返回 `false`。
     */
    [[nodiscard]] auto update(SelectedTargetObservation observation) -> bool {
        const bool targetChanged = current_.target != observation.target;
        current_ = std::move(observation);
        return targetChanged;
    }

    /**
     * @brief 获取最近一次观测结果。
     * @return 当前观测结果的只读引用。
     */
    [[nodiscard]] auto current() const -> const SelectedTargetObservation& {
        return current_;
    }

private:
    /** @brief 最近一次写入的目标观测结果。 */
    SelectedTargetObservation current_;
};

/**
 * @brief 判断排队的编辑请求是否仍指向当前待出战帕鲁。
 * @param[in] request 待执行编辑请求。
 * @param[in] current 当前选中目标。
 * @return 两者目标均有效且句柄相同时返回 `true`。
 */
[[nodiscard]] inline auto request_targets_current(
    const SkillEditRequest& request, const SelectedTargetObservation& current) -> bool {
    return current.target != 0 && request.target == current.target;
}

/**
 * @brief 仅在排队请求仍指向当前待出战帕鲁时调用写入回调。
 * @tparam Apply 接受 `const SkillEditRequest&` 并返回 `SkillEditResult` 的可调用类型。
 * @param[in] request 待执行的排队请求。
 * @param[in] current 执行时重新观测到的当前目标。
 * @param[in] apply 真正执行技能写入的回调。
 * @return 目标一致时返回回调结果；目标已切换或为空时返回 `std::nullopt`。
 */
template <typename Apply>
[[nodiscard]] auto apply_if_target_is_current(
    const SkillEditRequest& request, const SelectedTargetObservation& current,
    Apply&& apply) -> std::optional<SkillEditResult> {
    if (!request_targets_current(request, current)) {
        return std::nullopt;
    }

    return std::invoke(std::forward<Apply>(apply), request);
}

}  // namespace skill_editor
