/**
 * @file selected_target_state.hpp
 * @brief 定义当前待出战帕鲁目标的纯值状态和请求目标校验。
 */
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

#include <skills/skill_editor_service.hpp>

/** @brief 提供当前待出战帕鲁目标的纯 C++ 状态管理。 */
namespace skill_editor {

/**
 * @brief 不依赖 Unreal 类型的帕鲁个体唯一标识。
 */
struct TargetIdentity {
    /** @brief `FPalInstanceID.InstanceId` 的四个 32 位组成部分。 */
    std::array<std::uint32_t, 4> instanceId{};

    /**
     * @brief 判断实例 GUID 是否非零。
     * @retval true 至少一个 GUID 分量非零。
     * @retval false 四个 GUID 分量均为零。
     */
    [[nodiscard]] auto is_valid() const -> bool {
        return std::ranges::any_of(instanceId, [](const auto value) { return value != 0; });
    }

    /** @brief 按四个 GUID 分量比较两个个体身份。 */
    auto operator==(const TargetIdentity&) const -> bool = default;
};

/**
 * @brief 当前待出战帕鲁的一次纯值观测结果。
 */
struct SelectedTargetObservation {
    /** @brief 当前帕鲁的稳定个体身份。 */
    TargetIdentity identity;

    /** @brief 用于界面展示的 CharacterID 或名称。 */
    std::string name;

    /**
     * @brief 判断观测是否包含有效个体身份。
     * @return `identity.is_valid()` 的结果。
     */
    [[nodiscard]] auto is_valid() const -> bool {
        return identity.is_valid();
    }
};

/**
 * @brief 当前帕鲁运行时解析链的终止状态。
 */
enum class SelectedTargetResolutionStatus {
    success,                           /**< 已取得参数对象、个体 GUID 和 CharacterID。 */
    worldContextUnavailable,           /**< 未找到本地 PlayerController。 */
    palUtilityUnavailable,             /**< 未找到 PalUtility 默认对象。 */
    getHolderFunctionUnavailable,      /**< 未找到 GetOtomoHolderComponent。 */
    holderUnavailable,                 /**< 未取得当前玩家的 Otomo Holder。 */
    getSelectedFunctionUnavailable,    /**< 未找到 GetSelectedOtomoID。 */
    selectedSlotUnavailable,           /**< 当前槽位索引无效。 */
    getHandleFunctionUnavailable,      /**< 未找到 GetOtomoIndividualHandle。 */
    handleUnavailable,                 /**< 当前槽位没有个体 Handle。 */
    getParameterFunctionUnavailable,   /**< 未找到 TryGetIndividualParameter。 */
    parameterUnavailable,              /**< Handle 没有有效个体参数对象。 */
    parameterClassUnavailable,         /**< 参数对象类型与预期不符。 */
    getPalIdFunctionUnavailable,       /**< 未找到 GetPalId。 */
    individualIdUnavailable,           /**< GetPalId 返回的实例 GUID 无效。 */
    getCharacterIdFunctionUnavailable, /**< 未找到 GetCharacterID。 */
};

/**
 * @brief 把解析状态转换为可直接显示的中文诊断。
 * @param[in] status 当前解析状态。
 * @return 成功时为空；失败时返回具体失败阶段。
 */
[[nodiscard]] inline auto resolution_status_message(const SelectedTargetResolutionStatus status)
    -> std::string_view {
    using enum SelectedTargetResolutionStatus;
    switch (status) {
        case success:
            return {};
        case worldContextUnavailable:
            return "未找到本地 PlayerController";
        case palUtilityUnavailable:
            return "未找到 PalUtility";
        case getHolderFunctionUnavailable:
            return "未找到 GetOtomoHolderComponent";
        case holderUnavailable:
            return "未取得当前玩家的 Otomo Holder";
        case getSelectedFunctionUnavailable:
            return "未找到 GetSelectedOtomoID";
        case selectedSlotUnavailable:
            return "当前 Q/E 槽位没有有效帕鲁";
        case getHandleFunctionUnavailable:
            return "未找到 GetOtomoIndividualHandle";
        case handleUnavailable:
            return "未取得当前帕鲁的 IndividualHandle";
        case getParameterFunctionUnavailable:
            return "未找到 TryGetIndividualParameter";
        case parameterUnavailable:
            return "未取得当前帕鲁的 IndividualParameter";
        case parameterClassUnavailable:
            return "当前帕鲁参数对象类型无效";
        case getPalIdFunctionUnavailable:
            return "未找到 GetPalId";
        case individualIdUnavailable:
            return "当前帕鲁的 InstanceId 无效";
        case getCharacterIdFunctionUnavailable:
            return "未找到 GetCharacterID";
    }
    return "未知目标解析错误";
}

/**
 * @brief 从候选集合中选择第一个有效且属于本地玩家的值。
 * @tparam Range 可输入遍历的候选集合类型。
 * @tparam IsValid 接受候选值并判断其是否可用的谓词。
 * @tparam IsLocal 接受候选值并判断其是否属于本地玩家的谓词。
 * @param[in] candidates 按运行时发现顺序排列的候选值。
 * @param[in] isValid 候选有效性谓词。
 * @param[in] isLocal 本地玩家谓词。
 * @return 第一个同时满足两个谓词的候选；不存在时返回 std::nullopt。
 */
template <std::ranges::input_range Range, typename IsValid, typename IsLocal>
[[nodiscard]] auto find_local_candidate(const Range& candidates, IsValid&& isValid,
                                        IsLocal&& isLocal)
    -> std::optional<std::ranges::range_value_t<Range>> {
    for (const auto& candidate : candidates) {
        if (std::invoke(isValid, candidate) && std::invoke(isLocal, candidate)) {
            return candidate;
        }
    }
    return std::nullopt;
}

/**
 * @brief 保存用户显式确认的目标身份，并用代数保护排队请求。
 */
class SelectedTargetState {
public:
    /**
     * @brief 显式确认当前观测为技能编辑目标。
     * @param[in] observation 当前帧的纯值目标观测。
     * @retval true 观测有效且已保存为目标。
     * @retval false 观测没有有效实例 GUID，状态未改变。
     */
    [[nodiscard]] auto confirm(SelectedTargetObservation observation) -> bool {
        if (!observation.is_valid()) {
            return false;
        }

        current_ = std::move(observation);
        selected_ = true;
        ++generation_;
        return true;
    }

    /**
     * @brief 在已确认目标与当前 Q/E 观测不同时取消选择。
     * @param[in] observation 当前帧的纯值目标观测。
     * @retval true 已确认目标失效并被清空。
     * @retval false 尚未选择目标，或当前观测仍指向同一个体。
     */
    [[nodiscard]] auto invalidate_if_changed(const SelectedTargetObservation& observation) -> bool {
        if (!selected_) {
            return false;
        }
        if (!observation.is_valid() || current_.identity != observation.identity) {
            invalidate();
            return true;
        }

        current_.name = observation.name;
        return false;
    }

    /**
     * @brief 取消当前确认目标。
     * @details 仅当当前确有目标时增加代数，避免重复空状态产生无意义变化。
     */
    auto invalidate() -> void {
        if (!selected_) {
            return;
        }
        current_ = {};
        selected_ = false;
        ++generation_;
    }

    /**
     * @brief 查询是否已有用户显式确认的目标。
     * @return 当前选择状态。
     */
    [[nodiscard]] auto is_selected() const -> bool {
        return selected_;
    }

    /**
     * @brief 获取当前目标代数。
     * @return 每次确认或失效时递增的代数。
     */
    [[nodiscard]] auto generation() const -> std::uint64_t {
        return generation_;
    }

    /**
     * @brief 获取当前显式确认的纯值目标。
     * @return 未选择时为空观测；已选择时为最近一次同身份观测。
     */
    [[nodiscard]] auto current() const -> const SelectedTargetObservation& {
        return current_;
    }

    /**
     * @brief 校验请求代数和当前 Q/E 观测仍指向已确认个体。
     * @param[in] generation GUI 提交请求时取得的目标代数。
     * @param[in] observation 执行前在游戏线程重新解析的目标。
     * @return 已选择、代数相同且实例 GUID 相同时返回 true。
     */
    [[nodiscard]] auto matches(const std::uint64_t generation,
                               const SelectedTargetObservation& observation) const -> bool {
        return selected_ && generation == generation_ && observation.is_valid() &&
               current_.identity == observation.identity;
    }

private:
    SelectedTargetObservation current_; /**< 当前显式确认的纯值目标。 */
    std::uint64_t generation_{};        /**< 确认或失效时递增的请求防护代数。 */
    bool selected_{};                   /**< 是否存在用户显式确认的目标。 */
};

/**
 * @brief 仅在排队请求仍指向当前待出战帕鲁时调用写入回调。
 * @tparam Apply 接受 `const SkillEditRequest&` 并返回 `SkillEditResult` 的可调用类型。
 * @param[in] request 待执行的排队请求。
 * @param[in] state 用户显式确认的目标身份与代数状态。
 * @param[in] observation 执行时在游戏线程重新解析出的纯值目标。
 * @param[in] transientTarget 仅在当前帧有效、通过观测取得的 Unreal 目标句柄。
 * @param[in] apply 真正执行技能写入的回调。
 * @return 目标一致时返回回调结果；目标已切换或为空时返回 `std::nullopt`。
 */
template <typename Apply>
[[nodiscard]] auto apply_if_target_is_current(const SkillEditRequest& request,
                                              const SelectedTargetState& state,
                                              const SelectedTargetObservation& observation,
                                              const SkillTarget transientTarget, Apply&& apply)
    -> std::optional<SkillEditResult> {
    if (transientTarget == 0 || !state.matches(request.targetGeneration, observation)) {
        return std::nullopt;
    }

    auto executableRequest = request;
    executableRequest.target = transientTarget;
    return std::invoke(std::forward<Apply>(apply), executableRequest);
}

}  // namespace skill_editor
