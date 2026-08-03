/**
 * @file pal_identity_editor.hpp
 * @brief 与 Unreal 解耦的帕鲁 Alpha、Lucky 与觉醒编辑领域模型。
 * @details 三个维度彼此独立；本文件只保存纯值快照和世界绑定请求，不保存 UObject。
 */
#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace pal_identity {
/** @brief 由非拥有 UObject 指针编码的当帧个体参数句柄。 */
using PalIdentityTarget = std::uintptr_t;
/** @brief 期望修改的三个独立个体维度；空 optional 表示不修改。 */
struct PalIdentityValues {
    std::optional<bool> alpha;     /**< 是否使用同物种的 Boss/Alpha CharacterID。 */
    std::optional<bool> lucky;     /**< `SaveParameter.IsRarePal`。 */
    std::optional<bool> awakening; /**< `SaveParameter.bIsAwakening`。 */
};

/** @return 请求是否至少包含一项修改。 */
[[nodiscard]] constexpr auto has_any_change(const PalIdentityValues& values) noexcept -> bool {
    return values.alpha.has_value() || values.lucky.has_value() || values.awakening.has_value();
}

/** @brief GUI 提交、等待游戏线程执行的一次身份/形态编辑。 */
struct PalIdentityEditRequest {
    PalIdentityValues values;
    std::uint64_t targetGeneration{};
    std::uint64_t worldGeneration{};
};

/** @brief 从游戏重读的 Alpha、Lucky 与觉醒纯值状态。 */
struct PalIdentitySnapshot {
    std::string characterId;      /**< 当前真实 CharacterID。 */
    std::string baseCharacterId;  /**< 经原生数据库验证的普通 CharacterID。 */
    std::string alphaCharacterId; /**< 经原生数据库验证的 Alpha CharacterID。 */
    bool alpha{};
    bool lucky{};
    bool awakening{};
    bool alphaAvailable{};  /**< 当前物种是否存在可安全切换的普通/Alpha 配对。 */
    bool spawnStateKnown{}; /**< 是否由队伍 Holder 成功读取权威出战状态。 */
    bool summoned{};        /**< 当前选中 Handle 是否为 Holder 报告的出战 Handle。 */
    bool readable{};
};

/** @return 可读快照中的所有请求维度是否等于期望值。 */
[[nodiscard]] constexpr auto verify_identity_edit(const PalIdentityValues& expected,
                                                  const PalIdentitySnapshot& actual) noexcept
    -> bool {
    return actual.readable && (!expected.alpha.has_value() || actual.alpha == *expected.alpha) &&
           (!expected.lucky.has_value() || actual.lucky == *expected.lucky) &&
           (!expected.awakening.has_value() || actual.awakening == *expected.awakening);
}

/** @brief 一次身份事务的最终状态。 */
enum class PalIdentityEditStatus {
    succeeded,
    rejected,
    preflightFailed,
    verificationFailed,
    rollbackFailed,
};

/** @brief 身份事务结果及事务结束后的真实重读状态。 */
struct PalIdentityEditResult {
    PalIdentityEditStatus status{PalIdentityEditStatus::rejected};
    PalIdentitySnapshot snapshot;
    std::string message;
};

/** @brief GUI 可直接编辑的三维状态。 */
struct PalIdentityEditableValues {
    bool alpha{};
    bool lucky{};
    bool awakening{};
};

/** @brief 以游戏快照为基线，只生成用户实际改变的维度。 */
class PalIdentityEditDraft final {
public:
    auto synchronize(const PalIdentitySnapshot& snapshot,
                     const std::uint64_t targetGeneration) noexcept -> void {
        if (!snapshot.readable) {
            reset();
            return;
        }
        values_ = {
            .alpha = snapshot.alpha,
            .lucky = snapshot.lucky,
            .awakening = snapshot.awakening,
        };
        baseline_ = values_;
        targetGeneration_ = targetGeneration;
        initialized_ = true;
    }

    auto reset() noexcept -> void {
        values_ = {};
        baseline_ = {};
        targetGeneration_ = 0;
        initialized_ = false;
    }

    [[nodiscard]] auto values() noexcept -> PalIdentityEditableValues& {
        return values_;
    }

    [[nodiscard]] auto values() const noexcept -> const PalIdentityEditableValues& {
        return values_;
    }

    [[nodiscard]] auto initialized_for(const std::uint64_t targetGeneration) const noexcept
        -> bool {
        return initialized_ && targetGeneration_ == targetGeneration;
    }

    auto reconcile(const PalIdentitySnapshot& snapshot,
                   const std::uint64_t targetGeneration) noexcept -> void {
        if (!snapshot.readable) {
            return;
        }
        if (!initialized_for(targetGeneration)) {
            synchronize(snapshot, targetGeneration);
            return;
        }
        const auto mergeField = [](bool& value, bool& baseline, const bool gameValue) {
            const bool locallyChanged = value != baseline;
            baseline = gameValue;
            if (!locallyChanged || value == gameValue) {
                value = gameValue;
            }
        };
        mergeField(values_.alpha, baseline_.alpha, snapshot.alpha);
        mergeField(values_.lucky, baseline_.lucky, snapshot.lucky);
        mergeField(values_.awakening, baseline_.awakening, snapshot.awakening);
    }

    [[nodiscard]] auto make_request(const std::uint64_t worldGeneration) const
        -> std::optional<PalIdentityEditRequest> {
        if (!initialized_) {
            return std::nullopt;
        }
        PalIdentityValues changes;
        if (values_.alpha != baseline_.alpha) {
            changes.alpha = values_.alpha;
        }
        if (values_.lucky != baseline_.lucky) {
            changes.lucky = values_.lucky;
        }
        if (values_.awakening != baseline_.awakening) {
            changes.awakening = values_.awakening;
        }
        if (!has_any_change(changes)) {
            return std::nullopt;
        }
        return PalIdentityEditRequest{
            .values = std::move(changes),
            .targetGeneration = targetGeneration_,
            .worldGeneration = worldGeneration,
        };
    }

private:
    PalIdentityEditableValues values_{};
    PalIdentityEditableValues baseline_{};
    std::uint64_t targetGeneration_{};
    bool initialized_{};
};

/** @brief GUI 单生产者、游戏线程单消费者的最新身份请求槽。 */
class PalIdentityEditRequestSlot final {
public:
    auto submit(PalIdentityEditRequest request) -> void {
        const std::lock_guard lock{mutex_};
        request_ = std::move(request);
    }

    [[nodiscard]] auto consume() -> std::optional<PalIdentityEditRequest> {
        const std::lock_guard lock{mutex_};
        return std::exchange(request_, std::nullopt);
    }

    auto clear() -> void {
        const std::lock_guard lock{mutex_};
        request_.reset();
    }

    [[nodiscard]] auto has_pending() const -> bool {
        const std::lock_guard lock{mutex_};
        return request_.has_value();
    }

private:
    mutable std::mutex mutex_;
    std::optional<PalIdentityEditRequest> request_;
};
}  // namespace pal_identity
