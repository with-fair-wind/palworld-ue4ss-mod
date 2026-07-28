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
enum class CooldownWork : std::uint8_t {
    none,    /**< 当前不需要扫描或写入。 */
    apply,   /**< 扫描一次严格匹配的爪钩对象并建立覆盖。 */
    restore, /**< 按账本恢复本 mod 实际覆盖的对象。 */
};

/** @brief 一次网关应用的领域结果；目标未加载与结构性失败必须区别处理。 */
enum class CooldownApplyOutcome : std::uint8_t {
    succeeded,
    targetUnavailable,
    terminalFailure,
};

/** @brief GUI 与生命周期诊断使用的纯值运行阶段。 */
enum class CooldownRuntimePhase : std::uint8_t {
    off,
    waitingForWorld,
    readyToApply,
    applying,
    active,
    waitingForRetry,
    restoring,
    safetyDisabled,
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
        applyInFlight_ = false;
        retryRequired_ = false;
        safetyDisabled_ = false;
        restoreAttempted_ = false;
        return true;
    }

    /** @brief 更新用户期望；关闭时不会丢弃恢复账本。 */
    auto set_desired(const bool enabled) noexcept -> void {
        if (desired_ == enabled) {
            return;
        }
        desired_ = enabled;
        if (enabled && records_.empty() && !safetyDisabled_) {
            applyInFlight_ = false;
            retryRequired_ = false;
        } else if (!enabled && records_.empty()) {
            retryRequired_ = false;
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
        if (desired_ && records_.empty() && !applyInFlight_ && !retryRequired_ &&
            !safetyDisabled_) {
            return CooldownWork::apply;
        }
        return CooldownWork::none;
    }

    /**
     * @brief 在调用同步网关前占用一次应用工作。
     * @retval true 代次和状态允许开始。
     * @retval false 请求已过期、等待显式重试、已有覆盖或本世界已安全禁用。
     */
    [[nodiscard]] auto begin_apply(const std::uint64_t worldGeneration) noexcept -> bool {
        if (worldGeneration != worldGeneration_ || !desired_ || applyInFlight_ ||
            retryRequired_ || safetyDisabled_ || !records_.empty()) {
            return false;
        }
        applyInFlight_ = true;
        return true;
    }

    /**
     * @brief 完成当前同步应用，并根据结果进入活动、等待重试或本世界安全禁用。
     * @param[in] worldGeneration 应用开始时绑定的世界代次。
     * @param[in] outcome 网关分类后的领域结果。
     * @param[in] records 成功覆盖对象的原值；只有 succeeded 时必须非空。
     */
    [[nodiscard]] auto complete_apply(
        const std::uint64_t worldGeneration, const CooldownApplyOutcome outcome,
        std::vector<CooldownOverrideRecord> records = {}) noexcept -> bool {
        if (worldGeneration != worldGeneration_ || !applyInFlight_) {
            return false;
        }

        applyInFlight_ = false;
        switch (outcome) {
        case CooldownApplyOutcome::succeeded:
            if (records.empty()) {
                safetyDisabled_ = true;
                return false;
            }
            records_ = std::move(records);
            retryRequired_ = false;
            return true;
        case CooldownApplyOutcome::targetUnavailable:
            records_.clear();
            retryRequired_ = true;
            return true;
        case CooldownApplyOutcome::terminalFailure:
            records_.clear();
            retryRequired_ = false;
            safetyDisabled_ = true;
            return true;
        }
        safetyDisabled_ = true;
        return false;
    }

    /**
     * @brief 授权目标未加载后的下一次单次扫描。
     * @retval true 当前代次处于等待重试且已重新开放 apply。
     */
    [[nodiscard]] auto request_retry(const std::uint64_t worldGeneration) noexcept -> bool {
        if (worldGeneration != worldGeneration_ || !desired_ || !retryRequired_ ||
            safetyDisabled_ || applyInFlight_ || !records_.empty()) {
            return false;
        }
        retryRequired_ = false;
        return true;
    }

    /** @brief 返回不访问 Unreal 的当前领域阶段。 */
    [[nodiscard]] auto phase(const std::uint64_t worldGeneration) const noexcept
        -> CooldownRuntimePhase {
        if (worldGeneration != worldGeneration_) {
            return desired_ ? CooldownRuntimePhase::waitingForWorld
                            : CooldownRuntimePhase::off;
        }
        if (safetyDisabled_) {
            return CooldownRuntimePhase::safetyDisabled;
        }
        if (!desired_) {
            return records_.empty() ? CooldownRuntimePhase::off
                                    : CooldownRuntimePhase::restoring;
        }
        if (restoreAttempted_) {
            return CooldownRuntimePhase::restoring;
        }
        if (applyInFlight_) {
            return CooldownRuntimePhase::applying;
        }
        if (!records_.empty()) {
            return CooldownRuntimePhase::active;
        }
        if (retryRequired_) {
            return CooldownRuntimePhase::waitingForRetry;
        }
        return CooldownRuntimePhase::readyToApply;
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
    bool applyInFlight_{};                        /**< 同步网关调用是否已占用应用工作。 */
    bool retryRequired_{};                        /**< 目标未加载后是否等待显式重试。 */
    bool safetyDisabled_{};                       /**< 结构或验证失败后阻止本世界再次覆盖。 */
    bool restoreAttempted_{};                     /**< 防止失败恢复退化为逐帧重试。 */
};

}  // namespace grappling_hook
