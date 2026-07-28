/**
 * @file cooldown_service.hpp
 * @brief 与 Unreal 解耦的爪钩枪冷却覆盖状态机和原值恢复账本。
 */
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/** @brief 提供爪钩枪冷却覆盖的纯值领域逻辑。 */
namespace grappling_hook {

/** @brief 游戏线程下一次需要执行的有限工作。 */
enum class CooldownWork {
    none,    /**< 当前不需要扫描或写入。 */
    apply,   /**< 扫描一次严格匹配的爪钩对象并建立覆盖。 */
    restore, /**< 按账本恢复本 mod 实际覆盖的对象。 */
};

/** @brief 一个被本 mod 覆盖对象的可恢复纯值记录。 */
struct CooldownOverrideRecord {
    std::wstring objectFullName; /**< UObject 完整名称；不保存对象指针。 */
    float originalCooldown{};    /**< 覆盖前从该对象读取的原始冷却时间。 */

    /** @brief 比较对象路径与原始值。 */
    auto operator==(const CooldownOverrideRecord&) const -> bool = default;
};

/**
 * @brief 判断物品 Raw ID 是否为当前版本或旧版命名的正式爪钩枪。
 * @details 采用精确白名单；明确排除 `AirGrapplingGun` 等测试物品和名称仅含 Grappling 的其他对象。
 */
[[nodiscard]] inline auto is_grappling_item_id(const std::string_view itemId) noexcept -> bool {
    static constexpr std::array knownIds{
        std::string_view{"GrapplingGun"},   std::string_view{"GrapplingGun2"},
        std::string_view{"GrapplingGun3"},  std::string_view{"GrapplingGun4"},
        std::string_view{"GrapplingGun5"},  std::string_view{"GrapplingGun_1"},
        std::string_view{"GrapplingGun_2"}, std::string_view{"GrapplingGun_3"},
        std::string_view{"GrapplingGun_4"}, std::string_view{"GrapplingGun_5"},
    };
    for (const auto knownId : knownIds) {
        if (itemId == knownId) {
            return true;
        }
    }
    return false;
}

/**
 * @brief 管理期望状态、按世界应用次数和可逆覆盖账本。
 * @details 默认关闭时 `next_work()` 恒为 `none`；重复开启不会重复扫描或覆盖。
 */
class CooldownOverrideLedger {
public:
    /**
     * @brief 开始新的世界代次。
     * @retval true 没有遗留覆盖，已切换代次并允许按需应用。
     * @retval false 仍有必须先恢复的覆盖记录，状态保持不变。
     */
    [[nodiscard]] auto begin_world(const std::uint64_t worldGeneration) noexcept -> bool {
        if (!records_.empty()) {
            return false;
        }
        worldGeneration_ = worldGeneration;
        applyAttempted_ = false;
        restoreAttempted_ = false;
        return true;
    }

    /** @brief 更新用户期望；关闭时不会丢弃恢复账本。 */
    auto set_desired(const bool enabled) noexcept -> void {
        if (desired_ == enabled) {
            return;
        }
        desired_ = enabled;
        if (enabled && records_.empty()) {
            applyAttempted_ = false;
        }
    }

    /** @return 当前用户是否期望无冷却。 */
    [[nodiscard]] auto desired() const noexcept -> bool {
        return desired_;
    }

    /**
     * @brief 根据世界状态决定下一项一次性工作。
     * @return 世界不可访问或代次不匹配时返回 `none`；否则返回 apply/restore/none。
     */
    [[nodiscard]] auto next_work(const std::uint64_t worldGeneration,
                                 const bool worldAccessible) const noexcept -> CooldownWork {
        if (!worldAccessible || worldGeneration != worldGeneration_) {
            return CooldownWork::none;
        }
        if (!desired_ && !records_.empty() && !restoreAttempted_) {
            return CooldownWork::restore;
        }
        if (desired_ && records_.empty() && !applyAttempted_) {
            return CooldownWork::apply;
        }
        return CooldownWork::none;
    }

    /**
     * @brief 记录一次应用尝试及网关返回的全部原值。
     * @retval true 代次匹配且当前没有活动覆盖，记录已接收。
     * @retval false 请求已过期或已有活动覆盖，调用方不得覆盖现有账本。
     */
    [[nodiscard]] auto mark_apply_attempted(const std::uint64_t worldGeneration,
                                            std::vector<CooldownOverrideRecord> records) noexcept
        -> bool {
        if (worldGeneration != worldGeneration_ || !records_.empty()) {
            return false;
        }
        applyAttempted_ = true;
        records_ = std::move(records);
        return true;
    }

    /**
     * @brief 完成一次恢复尝试。
     * @param[in] succeeded 只有所有仍存在对象均恢复并验证成功时为 true。
     */
    auto complete_restore(const bool succeeded) noexcept -> void {
        restoreAttempted_ = true;
        if (succeeded) {
            records_.clear();
            restoreAttempted_ = false;
        }
    }

    /** @return 当前恢复账本的只读视图。 */
    [[nodiscard]] auto records() const noexcept -> std::span<const CooldownOverrideRecord> {
        return records_;
    }

private:
    std::vector<CooldownOverrideRecord> records_; /**< 本 mod 当前世界实际覆盖的对象原值。 */
    std::uint64_t worldGeneration_{};             /**< 账本绑定的世界代次。 */
    bool desired_{};                              /**< 用户期望的开关状态，默认关闭。 */
    bool applyAttempted_{};                       /**< 当前世界是否已经执行过一次应用尝试。 */
    bool restoreAttempted_{};                     /**< 防止失败恢复退化为逐帧重试。 */
};

}  // namespace grappling_hook
