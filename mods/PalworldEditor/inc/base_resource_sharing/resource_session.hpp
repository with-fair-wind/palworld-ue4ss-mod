#pragma once

#include <algorithm>
#include <cstdint>

#include <base_resource_sharing/resource_pool.hpp>

namespace base_resource_sharing {
inline constexpr float kCatalogRetrySeconds = 1.0F;
inline constexpr float kCatalogReconcileSeconds = 8.0F;

struct ResourceToggleTransition {
    bool disableRuntime{};
    bool beginAccessibleWorld{};
};

[[nodiscard]] constexpr auto decide_resource_toggle(const bool wasEnabled,
                                                    const bool requestedEnabled,
                                                    const bool worldAccessible) noexcept
    -> ResourceToggleTransition {
    if (wasEnabled == requestedEnabled) {
        return {};
    }
    if (!requestedEnabled) {
        return {.disableRuntime = true};
    }
    return {.beginAccessibleWorld = worldAccessible};
}

/** @brief 判断首次资格回调是否获准同步执行一次目录 bootstrap。 */
[[nodiscard]] constexpr auto should_bootstrap_catalog(
    const bool enabled, const bool worldAccessible, const bool capabilityReady,
    const bool catalogReady, const std::uint64_t currentGeneration,
    const std::uint64_t requestedGeneration) noexcept -> bool {
    return enabled && worldAccessible && capabilityReady && !catalogReady &&
           currentGeneration == requestedGeneration;
}

class ReconcileScheduler {
public:
    auto begin_world(const std::uint64_t generation) noexcept -> void {
        generation_ = generation;
        pending_ = true;
        inFlight_ = false;
        elapsedSinceSuccess_ = 0.0F;
        retryRemaining_ = 0.0F;
    }

    auto request_immediate(const std::uint64_t generation) noexcept -> void {
        if (generation != generation_) {
            return;
        }
        pending_ = true;
        retryRemaining_ = 0.0F;
    }

    [[nodiscard]] auto advance(const float deltaSeconds, const std::uint64_t generation,
                               const bool mayRun) noexcept -> bool {
        if (!mayRun || generation != generation_ || inFlight_) {
            return false;
        }

        const auto elapsed = std::max(deltaSeconds, 0.0F);
        if (pending_) {
            retryRemaining_ = std::max(retryRemaining_ - elapsed, 0.0F);
            if (retryRemaining_ > 0.0F) {
                return false;
            }
        } else {
            elapsedSinceSuccess_ += elapsed;
            if (elapsedSinceSuccess_ < kCatalogReconcileSeconds) {
                return false;
            }
        }

        pending_ = false;
        inFlight_ = true;
        return true;
    }

    auto complete(const bool success, const std::uint64_t generation) noexcept -> void {
        if (generation != generation_ || !inFlight_) {
            return;
        }
        inFlight_ = false;
        elapsedSinceSuccess_ = 0.0F;
        if (success) {
            retryRemaining_ = 0.0F;
        } else {
            pending_ = true;
            retryRemaining_ = kCatalogRetrySeconds;
        }
    }

    auto reset() noexcept -> void {
        generation_ = 0;
        pending_ = false;
        inFlight_ = false;
        elapsedSinceSuccess_ = 0.0F;
        retryRemaining_ = 0.0F;
    }

private:
    std::uint64_t generation_{};
    bool pending_{};
    bool inFlight_{};
    float elapsedSinceSuccess_{};
    float retryRemaining_{};
};

/** @brief 单一前台材料操作所有权的转换类别。 */
enum class ForegroundTransitionKind : std::uint8_t {
    none,
    acquired,
    refreshed,
    preempted,
    released,
};

/** @brief 描述一次前台操作所有权变化，不保存任何 Unreal 对象。 */
struct ForegroundTransition {
    ForegroundTransitionKind kind{ForegroundTransitionKind::none};
    std::optional<ResourceOperation> previous;
    std::optional<ResourceOperation> current;
};

/**
 * @brief 串行化制作与建造材料会话，确保同一时刻只有一个消费面拥有联合。
 * @details 制作与建造都由显式界面/模式关闭事件释放。
 */
class ForegroundMaterialSession {
public:
    /** @brief 切换世界代次并清空旧前台所有者。 */
    auto begin_world(const std::uint64_t generation) noexcept -> void {
        generation_ = generation;
        active_.reset();
    }

    /** @brief 获取前台所有权；不同操作会确定性抢占旧操作。 */
    [[nodiscard]] auto acquire(const ResourceOperation operation,
                               const std::uint64_t generation) noexcept -> ForegroundTransition {
        if (generation != generation_ || operation == ResourceOperation::repair) {
            return {};
        }
        if (!active_.has_value()) {
            active_ = operation;
            return {
                .kind = ForegroundTransitionKind::acquired,
                .current = operation,
            };
        }
        if (*active_ == operation) {
            return {
                .kind = ForegroundTransitionKind::refreshed,
                .previous = operation,
                .current = operation,
            };
        }

        const auto previous = active_;
        active_ = operation;
        return {
            .kind = ForegroundTransitionKind::preempted,
            .previous = previous,
            .current = operation,
        };
    }

    /** @brief 验证当前同类前台操作仍处于活动状态。 */
    [[nodiscard]] auto touch(const ResourceOperation operation,
                             const std::uint64_t generation) noexcept -> bool {
        if (generation != generation_ || active_ != operation) {
            return false;
        }
        return true;
    }

    /** @brief 仅允许当前所有者释放自身，过期或异类事件不影响状态。 */
    [[nodiscard]] auto release(const ResourceOperation operation,
                               const std::uint64_t generation) noexcept -> ForegroundTransition {
        if (generation != generation_ || active_ != operation) {
            return {};
        }
        const auto previous = active_;
        active_.reset();
        return {
            .kind = ForegroundTransitionKind::released,
            .previous = previous,
        };
    }

    /** @brief 保留统一的逐帧接口；前台会话只由显式关闭事件释放。 */
    [[nodiscard]] auto advance(const float deltaSeconds, const std::uint64_t generation) noexcept
        -> ForegroundTransition {
        static_cast<void>(deltaSeconds);
        static_cast<void>(generation);
        return {};
    }

    /** @return 匹配世界代次的当前前台操作。 */
    [[nodiscard]] auto active(const std::uint64_t generation) const noexcept
        -> std::optional<ResourceOperation> {
        return generation == generation_ ? active_ : std::nullopt;
    }

    /** @brief 清空所有代次和所有权状态。 */
    auto reset() noexcept -> void {
        generation_ = 0;
        active_.reset();
    }

private:
    std::uint64_t generation_{};
    std::optional<ResourceOperation> active_;
};

/** @brief 只以 GUID 和世界代次跟踪本地玩家当前所在据点。 */
class CurrentBaseState {
public:
    auto begin_world(const std::uint64_t generation) noexcept -> void {
        generation_ = generation;
        current_.reset();
    }

    [[nodiscard]] auto enter(const GuidKey baseId, const std::uint64_t generation) noexcept
        -> bool {
        if (generation != generation_ || !baseId.valid()) {
            return false;
        }
        current_ = baseId;
        return true;
    }

    [[nodiscard]] auto exit(const GuidKey baseId, const std::uint64_t generation) noexcept -> bool {
        if (generation != generation_ || current_ != baseId) {
            return false;
        }
        current_.reset();
        return true;
    }

    [[nodiscard]] auto current(const std::uint64_t generation) const noexcept
        -> std::optional<GuidKey> {
        return generation == generation_ ? current_ : std::nullopt;
    }

    auto reset() noexcept -> void {
        generation_ = 0;
        current_.reset();
    }

private:
    std::uint64_t generation_{};
    std::optional<GuidKey> current_;
};
}  // namespace base_resource_sharing
