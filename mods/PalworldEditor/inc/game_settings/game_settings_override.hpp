/**
 * @file game_settings_override.hpp
 * @brief 游戏参数覆盖的纯值层：参数目录与多字段账本。
 * @details 不包含任何 Unreal 头；全部覆盖作用于 UPalGameSetting 单例的直接
 *          数值字段。目录为编译期常量，账本使用与目录等长的并行数组。
 */
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

/** @brief 提供游戏参数覆盖的纯值领域逻辑。 */
namespace game_settings {

/** @brief 参数值：int32 或 float。 */
using OverrideValue = std::variant<std::int32_t, float>;

/** @brief 参数的显示分类。 */
enum class Category : std::uint8_t {
    party,         /**< 队伍与个体 */
    progression,   /**< 进度与浓缩 */
    world,         /**< 世界与时间 */
    qualityOfLife, /**< 便利性 */
};

/** @brief 参数分类的中文显示名。 */
[[nodiscard]] constexpr auto category_name(const Category c) noexcept -> std::string_view {
    switch (c) {
        case Category::party:
            return "队伍与个体";
        case Category::progression:
            return "进度与浓缩";
        case Category::world:
            return "世界与时间";
        case Category::qualityOfLife:
            return "便利性";
    }
    return "未知";
}

/** @brief 一条可覆盖参数的完整规格。 */
struct OverrideSpec {
    Category category;
    std::string_view propertyName; /**< UPalGameSetting 上的属性名。 */
    std::string_view displayName;  /**< GUI 中文显示名。 */
    std::string_view description;  /**< GUI 中文说明。 */
    OverrideValue defaultValue;    /**< 建议覆盖值。 */
    OverrideValue minValue;        /**< 安全域下限。 */
    OverrideValue maxValue;        /**< 安全域上限。 */
};

/** @brief 验证值是否在安全域内且类型匹配。 */
[[nodiscard]] constexpr auto is_value_safe(const OverrideSpec& spec, const OverrideValue& value)
    -> bool {
    if (value.index() != spec.defaultValue.index()) {
        return false;  // 类型不匹配
    }
    const auto in_range = [&spec, &value]() -> bool {
        if (const auto* const v = std::get_if<std::int32_t>(&value)) {
            const auto* const lo = std::get_if<std::int32_t>(&spec.minValue);
            const auto* const hi = std::get_if<std::int32_t>(&spec.maxValue);
            return lo != nullptr && hi != nullptr && *v >= *lo && *v <= *hi;
        }
        if (const auto* const v = std::get_if<float>(&value)) {
            const auto* const lo = std::get_if<float>(&spec.minValue);
            const auto* const hi = std::get_if<float>(&spec.maxValue);
            return lo != nullptr && hi != nullptr && *v >= *lo && *v <= *hi;
        }
        return false;
    }();
    return in_range;
}

/**
 * @brief UPalGameSetting 数值参数覆盖目录（编译期常量）。
 * @note 属性名与类型来自 Palworld 1.0.3 SDK dump；新增前须在 dump 中确认。
 */
inline constexpr std::array kOverrideCatalog{
    // ── 队伍与个体 ──
    OverrideSpec{Category::party, "OtomoSlotNum", "队伍槽位", "同时出战帕鲁数量", 5,
                 std::int32_t{1}, std::int32_t{10}},
    OverrideSpec{Category::party, "RarePal_AppearanceProbability", "稀有帕鲁出现率",
                 "闪光帕鲁出现概率（默认 0.02 = 2%）", 1.0F, 0.0F, 1.0F},
    OverrideSpec{Category::party, "PredatorPal_AppearanceProbability", "捕食者帕鲁出现率",
                 "捕食者帕鲁出现概率", 0.5F, 0.0F, 1.0F},
    OverrideSpec{Category::party, "RarePal_LevelAdd", "稀有帕鲁等级加成", "闪光帕鲁额外等级", 10,
                 std::int32_t{0}, std::int32_t{50}},
    OverrideSpec{Category::party, "BossOrRarePal_TalentMin", "Boss/稀有个体值下限",
                 "Boss 和稀有帕鲁的最低个体值", 50, std::int32_t{0}, std::int32_t{100}},
    // ── 进度与浓缩 ──
    OverrideSpec{Category::progression, "CharacterRankUpRequiredNumDefault", "浓缩消耗帕鲁数",
                 "升星默认消耗的同种帕鲁数量", 1, std::int32_t{1}, std::int32_t{100}},
    OverrideSpec{Category::progression, "CharacterMaxRank", "最大星级", "浓缩可达最大星级", 5,
                 std::int32_t{1}, std::int32_t{10}},
    OverrideSpec{Category::progression, "CharacterMaxLevel", "最大等级", "角色最大等级", 100,
                 std::int32_t{1}, std::int32_t{200}},
    // ── 世界与时间 ──
    OverrideSpec{Category::world, "PalWorldMinutes_RealOneMinute", "游戏时间流速",
                 "现实 1 分钟 = 游戏多少分钟（默认 60）", 120, std::int32_t{1}, std::int32_t{1440}},
    OverrideSpec{Category::world, "BaseCampAreaRange", "据点半径(cm)", "据点覆盖半径", 5000.0F,
                 1000.0F, 20000.0F},
    // ── 便利性 ──
    OverrideSpec{Category::qualityOfLife, "ReturnOtomoPalCoolTime", "收回帕鲁冷却",
                 "收回出战帕鲁的冷却时间（秒）", 0.0F, 0.0F, 30.0F},
    OverrideSpec{Category::qualityOfLife, "ConsumStamina_PalThrow", "投掷帕鲁消耗体力",
                 "投出帕鲁球消耗的体力", 0, std::int32_t{0}, std::int32_t{100}},
    OverrideSpec{Category::qualityOfLife, "WorkAmountBySecForPlayer", "玩家工作速度",
                 "玩家每秒工作量倍率", 10.0F, 0.1F, 100.0F},
};

/** @brief 目录大小常量。 */
inline constexpr std::size_t kOverrideCount = kOverrideCatalog.size();

/** @brief GUI 与生命周期诊断使用的纯值运行阶段。 */
enum class RuntimePhase : std::uint8_t {
    off,
    active,
    safetyDisabled,
};

/**
 * @brief 多字段覆盖账本：管理期望值与原值记录。
 * @details 与目录等长的并行数组；开启时按需写入，关闭/切图时按账本恢复。
 */
class OverrideLedger final {
public:
    OverrideLedger() = default;

    /** @brief 更新某参数的期望值；值未变化时无操作。 */
    auto set_desired(const std::size_t index, const OverrideValue& value) -> void {
        if (index >= kOverrideCount || !is_value_safe(kOverrideCatalog[index], value)) {
            return;
        }
        if (desired_[index].has_value() && *desired_[index] == value) {
            return;
        }
        desired_[index] = value;
    }

    /** @brief 清除某参数的期望（恢复原值）。 */
    auto clear_desired(const std::size_t index) -> void {
        if (index < kOverrideCount) {
            desired_[index].reset();
        }
    }

    /** @brief 清除全部期望。 */
    auto clear_all_desired() -> void {
        for (auto& d : desired_) {
            d.reset();
        }
    }

    /** @return 某参数的期望值；未设置返回空。 */
    [[nodiscard]] auto desired(const std::size_t index) const -> std::optional<OverrideValue> {
        return index < kOverrideCount ? desired_[index] : std::nullopt;
    }

    /** @brief 记录一次成功写入的原值。 */
    auto record_applied(const std::size_t index, const OverrideValue& original) -> void {
        if (index < kOverrideCount) {
            originals_[index] = original;
        }
    }

    /** @return 某参数的原值；无记录返回空。 */
    [[nodiscard]] auto original(const std::size_t index) const -> std::optional<OverrideValue> {
        return index < kOverrideCount ? originals_[index] : std::nullopt;
    }

    /** @brief 清除某参数的恢复责任。 */
    auto clear_record(const std::size_t index) -> void {
        if (index < kOverrideCount) {
            originals_[index].reset();
        }
    }

    /** @brief 清除全部恢复责任（世界重建，新 GameSetting 用原生值）。 */
    auto clear_all_records() -> void {
        for (auto& o : originals_) {
            o.reset();
        }
    }

    /** @return 当前有恢复责任的参数下标列表。 */
    [[nodiscard]] auto active_indices() const -> std::vector<std::size_t> {
        std::vector<std::size_t> result;
        for (std::size_t i{}; i < kOverrideCount; ++i) {
            if (originals_[i].has_value()) {
                result.push_back(i);
            }
        }
        return result;
    }

    /** @return 当前有期望但尚未写入的参数下标列表（期望有值但无原值记录）。 */
    [[nodiscard]] auto pending_indices() const -> std::vector<std::size_t> {
        std::vector<std::size_t> result;
        for (std::size_t i{}; i < kOverrideCount; ++i) {
            if (desired_[i].has_value() && !originals_[i].has_value()) {
                result.push_back(i);
            }
        }
        return result;
    }

    /** @return 当前有恢复责任但期望已清除的参数（需要写回原值）。 */
    [[nodiscard]] auto restoring_indices() const -> std::vector<std::size_t> {
        std::vector<std::size_t> result;
        for (std::size_t i{}; i < kOverrideCount; ++i) {
            if (!desired_[i].has_value() && originals_[i].has_value()) {
                result.push_back(i);
            }
        }
        return result;
    }

    /** @brief 标记结构性/回滚失败，本世界安全停用。 */
    auto disable_for_world() -> void {
        safetyDisabled_ = true;
    }

    /** @return 是否已安全停用。 */
    [[nodiscard]] auto safety_disabled() const -> bool {
        return safetyDisabled_;
    }

    /** @brief 新世界：解除安全停用、清空账本（期望保留供恢复）。 */
    auto begin_world() -> void {
        safetyDisabled_ = false;
        clear_all_records();
    }

    /** @return 是否有任何待写入或待恢复的工作。 */
    [[nodiscard]] auto has_work() const -> bool {
        return !pending_indices().empty() || !restoring_indices().empty();
    }

    /** @return 运行阶段。 */
    [[nodiscard]] auto phase() const -> RuntimePhase {
        if (safetyDisabled_) {
            return RuntimePhase::safetyDisabled;
        }
        return active_indices().empty() ? RuntimePhase::off : RuntimePhase::active;
    }

private:
    std::array<std::optional<OverrideValue>, kOverrideCount> desired_{};
    std::array<std::optional<OverrideValue>, kOverrideCount> originals_{};
    bool safetyDisabled_{};
};

}  // namespace game_settings
