/**
 * @file fishing_boost_service.hpp
 * @brief 钓鱼圣手的纯值配置与恢复账本。
 * @details 与 ReviveTimerLedger 同语义：单对象多字段的快照→写入→验证→恢复。
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

/** @brief 提供钓鱼圣手的纯值领域逻辑。 */
namespace fishing_boost {

/** @brief 要覆盖的字段数。 */
inline constexpr std::size_t kFieldCount = 4;

/** @brief 字段名与建议覆盖值（作用于 UPalFishingSystem.CatchBattleParameter）。 */
struct FieldSpec {
    const char* fieldName;
    float overrideValue;
    std::string_view description;
};

inline constexpr std::array<FieldSpec, kFieldCount> kFieldCatalog{{
    {"SinkWaitMinTime", 0.0F, "鱼咬钩最短等待(秒)→0"},
    {"SinkWaitMaxTime", 0.0F, "鱼咬钩最长等待(秒)→0"},
    {"RequiredCatchAmount", 1.0F, "捕获所需量→1(一次即满)"},
    {"DefaultProgressAmount", 999.0F, "每次输入进度→999"},
}};

/** @brief 世界子系统候选的纯值优先级。 */
struct SystemCandidateRank {
    bool matchesExpectedWorld{};  /**< 是否属于调用方提供的当前世界。 */
    std::int32_t internalIndex{}; /**< UObject 注册序号；较新的实例通常更大。 */
};

/**
 * @brief 判断候选是否应替换当前选择。
 * @param[in] candidate 已通过 UE4SS 类继承匹配、CDO 排除与 pending-kill 预检的候选。
 * @param[in] selected 当前选择；为空表示尚未找到候选。
 * @retval true 候选精确匹配预期世界，或在同等级中注册得更晚。
 * @retval false 当前选择优先级更高或相同。
 * @note 此函数只确定遍历中的暂定候选；调用方仍须用
 *       is_system_candidate_selection_unambiguous() 拒绝缺失或歧义的候选集合。
 */
[[nodiscard]] constexpr auto should_select_system_candidate(
    const SystemCandidateRank candidate, const std::optional<SystemCandidateRank> selected) noexcept
    -> bool {
    if (!selected.has_value()) {
        return true;
    }
    if (candidate.matchesExpectedWorld != selected->matchesExpectedWorld) {
        return candidate.matchesExpectedWorld;
    }
    return candidate.internalIndex > selected->internalIndex;
}

/**
 * @brief 判断运行时候选集合是否足以唯一确认目标子系统。
 * @param[in] hasExpectedWorld 调用方是否提供了有效的预期世界。
 * @param[in] candidateCount 通过对象生命周期预检的候选总数。
 * @param[in] matchingExpectedWorldCount 属于预期世界的候选数。
 * @retval true 有预期世界时恰有一个精确匹配；无预期世界时恰有一个有效候选。
 * @retval false 候选缺失或存在歧义，调用方必须 fail-closed 并保留恢复责任。
 * @note UObject InternalIndex 只用于让遍历结果确定化，不可作为世界身份依据。
 */
[[nodiscard]] constexpr auto is_system_candidate_selection_unambiguous(
    const bool hasExpectedWorld, const std::size_t candidateCount,
    const std::size_t matchingExpectedWorldCount) noexcept -> bool {
    return hasExpectedWorld ? matchingExpectedWorldCount == 1 : candidateCount == 1;
}

/** @brief 运行阶段。 */
enum class Phase : std::uint8_t {
    off,
    active,
    /** @brief 目标子系统持续不可用且重试耗尽：等待用户重新切换开关再次授权。 */
    waiting,
    safetyDisabled,
};

/**
 * @brief 钓鱼参数覆盖账本。
 * @details 开启时快照全部字段原值并写入覆盖值；关闭/切图时按账本恢复。
 */
class Ledger final {
public:
    auto set_desired(const bool enabled) noexcept -> void {
        desired_ = enabled;
    }
    [[nodiscard]] auto desired() const noexcept -> bool {
        return desired_;
    }

    auto record_originals(const std::array<float, kFieldCount>& values) -> void {
        originals_ = values;
        hasRecords_ = true;
        retiredFields_ = {};
    }
    [[nodiscard]] auto originals() const -> std::optional<std::array<float, kFieldCount>> {
        return hasRecords_ ? std::optional{originals_} : std::nullopt;
    }
    /** @brief 退役字段：恢复时发现值已被外部改走（不再等于覆盖值）时调用，
     *         该字段的恢复责任视为已消失，后续重试永久跳过。 */
    auto retire_field(const std::size_t index) -> void {
        if (index < kFieldCount) {
            retiredFields_[index] = true;
        }
    }
    [[nodiscard]] auto is_field_retired(const std::size_t index) const -> bool {
        return index < kFieldCount && retiredFields_[index];
    }
    auto clear_records() -> void {
        hasRecords_ = false;
        retiredFields_ = {};
    }
    [[nodiscard]] auto has_records() const -> bool {
        return hasRecords_;
    }

    auto disable_for_world() -> void {
        safetyDisabled_ = true;
    }
    [[nodiscard]] auto safety_disabled() const -> bool {
        return safetyDisabled_;
    }
    auto begin_world() -> void {
        safetyDisabled_ = false;
        clear_records();
    }

    [[nodiscard]] auto phase() const -> Phase {
        if (safetyDisabled_)
            return Phase::safetyDisabled;
        return hasRecords_ ? Phase::active : Phase::off;
    }

private:
    bool desired_{};
    std::array<float, kFieldCount> originals_{};
    std::array<bool, kFieldCount> retiredFields_{};
    bool hasRecords_{};
    bool safetyDisabled_{};
};

}  // namespace fishing_boost
