/**
 * @file fishing_boost_service.hpp
 * @brief 钓鱼圣手的纯值配置与恢复账本。
 * @details 与 ReviveTimerLedger 同语义：单对象多字段的快照→写入→验证→恢复。
 */
#pragma once

#include <array>
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
