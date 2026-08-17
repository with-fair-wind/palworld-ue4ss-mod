/**
 * @file stack_limit_service.hpp
 * @brief 与 Unreal 解耦的物品堆叠上限候选规则、运行阶段和恢复账本。
 */
#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

/** @brief 提供物品堆叠上限覆盖的纯值领域逻辑。 */
namespace item_stack_limit {

/** @brief Palworld 普通可堆叠物品的原生上限。 */
inline constexpr std::int32_t kNativeStackableLimit{9999};
/** @brief 本功能写入的高堆叠上限，保留在有符号 32 位整数安全域内。 */
inline constexpr std::int32_t kExpandedStackLimit{999999999};

/**
 * @brief 判断一个原始上限是否属于可安全扩展的普通可堆叠物品。
 * @note 上限为 1 或其他特殊值的装备、饰品和关键物品必须保持原样。
 */
[[nodiscard]] constexpr auto is_expandable_limit(const std::int32_t limit) noexcept -> bool {
    return limit == kNativeStackableLimit;
}

/** @brief 游戏线程下一次需要执行的一次性工作。 */
enum class StackLimitWork : std::uint8_t {
    none,
    apply,
    restore,
};

/** @brief 应用事务完成后交给领域账本的结果分类。 */
enum class StackLimitApplyOutcome : std::uint8_t {
    succeeded,
    targetUnavailable,
    preflightFailed,
    verifiedRollback,
    rollbackFailed,
};

/** @brief GUI 与生命周期诊断使用的纯值运行阶段。 */
enum class StackLimitRuntimePhase : std::uint8_t {
    off,
    readyToApply,
    applying,
    active,
    waitingForRetry,
    restoring,
    safetyDisabled,
};

/** @brief 一个由本 mod 实际覆盖的物品静态数据纯值记录。 */
struct StackLimitOverrideRecord {
    std::wstring objectFullName;                       /**< UObject 完整名称。 */
    std::string itemId;                                /**< 用于恢复时确认身份的 Raw ID。 */
    std::int32_t originalLimit{kNativeStackableLimit}; /**< 覆盖前读取的原值。 */

    /** @brief 比较对象身份和原值。 */
    auto operator==(const StackLimitOverrideRecord&) const -> bool = default;
};

/**
 * @brief 管理用户期望、世界代次、一次性工作和可逆覆盖账本。
 * @details 结构或回滚失败后安全停用应用路径；关闭和世界切换仍可继续尝试恢复。
 */
class StackLimitOverrideLedger {
public:
    /**
     * @brief 开始新的世界代次。
     * @param[in] worldGeneration 新世界代次。
     * @retval true 没有遗留恢复责任，已切换代次。
     * @retval false 仍有旧世界覆盖记录，调用方必须先继续恢复。
     */
    [[nodiscard]] auto begin_world(const std::uint64_t worldGeneration) noexcept -> bool {
        if (!records_.empty()) {
            return false;
        }
        worldGeneration_ = worldGeneration;
        applyInFlight_ = false;
        retryRequired_ = false;
        restoreAttempted_ = false;
        return true;
    }

    /**
     * @brief 更新用户期望；关闭开关不会丢弃恢复账本。
     * @param[in] enabled 是否期望启用高堆叠上限。
     */
    auto set_desired(const bool enabled) noexcept -> void {
        if (desired_ == enabled) {
            return;
        }
        desired_ = enabled;
        if (!enabled && records_.empty()) {
            retryRequired_ = false;
        } else if (enabled && records_.empty() && !safetyDisabled_) {
            applyInFlight_ = false;
            retryRequired_ = false;
        }
    }

    /** @return 用户当前是否期望扩大堆叠上限。 */
    [[nodiscard]] auto desired() const noexcept -> bool {
        return desired_;
    }

    /** @return 恢复失败后是否已永久停用本次运行的再次应用。 */
    [[nodiscard]] auto safety_disabled() const noexcept -> bool {
        return safetyDisabled_;
    }

    /**
     * @brief 根据世界状态返回下一项一次性工作。
     * @param[in] worldGeneration 当前世界代次。
     * @param[in] worldAccessible 当前是否允许访问 Unreal。
     * @return 下一项一次性工作；没有工作时返回 StackLimitWork::none。
     */
    [[nodiscard]] auto next_work(const std::uint64_t worldGeneration,
                                 const bool worldAccessible) const noexcept -> StackLimitWork {
        if (!worldAccessible || worldGeneration != worldGeneration_) {
            return StackLimitWork::none;
        }
        if (!desired_ && !records_.empty() && !restoreAttempted_) {
            return StackLimitWork::restore;
        }
        if (desired_ && records_.empty() && !applyInFlight_ && !retryRequired_ &&
            !safetyDisabled_) {
            return StackLimitWork::apply;
        }
        return StackLimitWork::none;
    }

    /**
     * @brief 在调用同步反射网关前占用一次应用工作。
     * @param[in] worldGeneration 当前世界代次。
     * @retval true 已占用本次工作。
     * @retval false 状态、代次或安全域不允许应用。
     */
    [[nodiscard]] auto begin_apply(const std::uint64_t worldGeneration) noexcept -> bool {
        if (worldGeneration != worldGeneration_ || !desired_ || applyInFlight_ || retryRequired_ ||
            safetyDisabled_ || !records_.empty()) {
            return false;
        }
        applyInFlight_ = true;
        return true;
    }

    /**
     * @brief 完成一次应用事务并保存成功记录或未恢复责任。
     * @param[in] worldGeneration 当前世界代次。
     * @param[in] outcome 反射事务结果分类。
     * @param[in] records 成功覆盖记录或回滚失败后剩余的恢复责任。
     * @return 调用是否与当前状态和世界代次匹配。
     */
    [[nodiscard]] auto complete_apply(const std::uint64_t worldGeneration,
                                      const StackLimitApplyOutcome outcome,
                                      std::vector<StackLimitOverrideRecord> records = {}) noexcept
        -> bool {
        if (worldGeneration != worldGeneration_ || !applyInFlight_) {
            return false;
        }
        applyInFlight_ = false;

        switch (outcome) {
            case StackLimitApplyOutcome::succeeded:
                if (records.empty()) {
                    desired_ = false;
                    safetyDisabled_ = true;
                    return false;
                }
                records_ = std::move(records);
                retryRequired_ = false;
                return true;
            case StackLimitApplyOutcome::targetUnavailable:
                retryRequired_ = true;
                return true;
            case StackLimitApplyOutcome::preflightFailed:
            case StackLimitApplyOutcome::verifiedRollback:
                desired_ = false;
                retryRequired_ = false;
                records_.clear();
                return true;
            case StackLimitApplyOutcome::rollbackFailed:
                desired_ = false;
                retryRequired_ = false;
                records_ = std::move(records);
                safetyDisabled_ = true;
                return !records_.empty();
        }
        desired_ = false;
        safetyDisabled_ = true;
        return false;
    }

    /** @brief 允许生命周期事件对遗留账本再执行一次恢复，不开放再次应用。 */
    auto allow_restore_retry() noexcept -> void {
        if (!records_.empty()) {
            restoreAttempted_ = false;
        }
    }

    /**
     * @brief 完成一次恢复；失败时只保留网关确认仍未解决的恢复责任。
     * @param[in] succeeded 网关是否恢复完全部记录。
     * @param[in] remainingRecords 网关确认仍需恢复的记录。
     * @retval true 结果自洽且已更新账本。
     * @retval false 结果不自洽；保留原账本并安全停用。
     */
    [[nodiscard]] auto complete_restore(
        const bool succeeded, std::vector<StackLimitOverrideRecord> remainingRecords = {}) noexcept
        -> bool {
        restoreAttempted_ = true;
        if (succeeded && remainingRecords.empty()) {
            records_.clear();
            restoreAttempted_ = false;
            return true;
        }
        if (!succeeded && !remainingRecords.empty()) {
            if (remainingRecords.size() > records_.size()) {
                safetyDisabled_ = true;
                return false;
            }
            for (const auto& record : remainingRecords) {
                if (std::ranges::find(records_, record) == records_.end()) {
                    safetyDisabled_ = true;
                    return false;
                }
            }
            records_ = std::move(remainingRecords);
            safetyDisabled_ = true;
            return true;
        }
        safetyDisabled_ = true;
        return false;
    }

    /**
     * @brief 返回不访问 Unreal 的当前运行阶段。
     * @param[in] worldGeneration 当前世界代次。
     * @return GUI 与生命周期诊断使用的运行阶段。
     */
    [[nodiscard]] auto phase(const std::uint64_t worldGeneration) const noexcept
        -> StackLimitRuntimePhase {
        if (worldGeneration != worldGeneration_) {
            return desired_ ? StackLimitRuntimePhase::readyToApply : StackLimitRuntimePhase::off;
        }
        if (!desired_) {
            if (!records_.empty()) {
                return StackLimitRuntimePhase::restoring;
            }
            return safetyDisabled_ ? StackLimitRuntimePhase::safetyDisabled
                                   : StackLimitRuntimePhase::off;
        }
        if (applyInFlight_) {
            return StackLimitRuntimePhase::applying;
        }
        if (!records_.empty()) {
            return StackLimitRuntimePhase::active;
        }
        if (retryRequired_) {
            return StackLimitRuntimePhase::waitingForRetry;
        }
        return safetyDisabled_ ? StackLimitRuntimePhase::safetyDisabled
                               : StackLimitRuntimePhase::readyToApply;
    }

    /** @return 当前仍由本 mod 负责恢复的纯值记录。 */
    [[nodiscard]] auto records() const noexcept -> std::span<const StackLimitOverrideRecord> {
        return records_;
    }

private:
    std::vector<StackLimitOverrideRecord> records_;
    std::uint64_t worldGeneration_{};
    bool desired_{};
    bool applyInFlight_{};
    bool retryRequired_{};
    bool restoreAttempted_{};
    bool safetyDisabled_{};
};

}  // namespace item_stack_limit
