#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include <base_resource_sharing/resource_pool.hpp>

namespace base_resource_sharing {
inline constexpr float kCatalogInitialRetrySeconds = 1.0F;
inline constexpr float kCatalogMaximumRetrySeconds = 30.0F;
inline constexpr float kCatalogReconcileSeconds = 60.0F;

/** @brief 一次资源目录发现对后续调度的分类。 */
enum class CatalogReconcileOutcome : std::uint8_t {
    complete,         /**< 目录完整可用，恢复低频兜底校准。 */
    partial,          /**< 至少一个容器瞬时未加载，按指数退避重试。 */
    structuralFailure /**< 反射结构或权限失败，只等待低频兜底或显式失效事件。 */
};

/**
 * @brief 根据结构错误和瞬时未加载容器数量分类目录发现结果。
 * @param[in] hasStructuralError 是否遇到不兼容结构、权限或无效数据。
 * @param[in] pendingContainerCount 暂未解析为活动容器的登记项数量。
 */
[[nodiscard]] constexpr auto classify_catalog_attempt(
    const bool hasStructuralError, const std::size_t pendingContainerCount) noexcept
    -> CatalogReconcileOutcome {
    if (hasStructuralError) {
        return CatalogReconcileOutcome::structuralFailure;
    }
    return pendingContainerCount == 0 ? CatalogReconcileOutcome::complete
                                      : CatalogReconcileOutcome::partial;
}

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
        nextRetrySeconds_ = kCatalogInitialRetrySeconds;
    }

    auto request_immediate(const std::uint64_t generation) noexcept -> void {
        if (generation != generation_) {
            return;
        }
        pending_ = true;
        retryRemaining_ = 0.0F;
        nextRetrySeconds_ = kCatalogInitialRetrySeconds;
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

    auto complete(const CatalogReconcileOutcome outcome, const std::uint64_t generation) noexcept
        -> void {
        if (generation != generation_ || !inFlight_) {
            return;
        }
        inFlight_ = false;
        elapsedSinceSuccess_ = 0.0F;
        switch (outcome) {
            case CatalogReconcileOutcome::complete:
                pending_ = false;
                retryRemaining_ = 0.0F;
                nextRetrySeconds_ = kCatalogInitialRetrySeconds;
                break;
            case CatalogReconcileOutcome::partial:
                pending_ = true;
                retryRemaining_ = nextRetrySeconds_;
                nextRetrySeconds_ = std::min(nextRetrySeconds_ * 2.0F, kCatalogMaximumRetrySeconds);
                break;
            case CatalogReconcileOutcome::structuralFailure:
                pending_ = false;
                retryRemaining_ = 0.0F;
                nextRetrySeconds_ = kCatalogInitialRetrySeconds;
                break;
        }
    }

    auto reset() noexcept -> void {
        generation_ = 0;
        pending_ = false;
        inFlight_ = false;
        elapsedSinceSuccess_ = 0.0F;
        retryRemaining_ = 0.0F;
        nextRetrySeconds_ = kCatalogInitialRetrySeconds;
    }

private:
    std::uint64_t generation_{};
    bool pending_{};
    bool inFlight_{};
    float elapsedSinceSuccess_{};
    float retryRemaining_{};
    float nextRetrySeconds_{kCatalogInitialRetrySeconds};
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
        buildingInventoryRefreshed_ = false;
    }

    /** @brief 获取前台所有权；不同操作会确定性抢占旧操作。 */
    [[nodiscard]] auto acquire(const ResourceOperation operation,
                               const std::uint64_t generation) noexcept -> ForegroundTransition {
        if (generation != generation_ || operation == ResourceOperation::repair) {
            return {};
        }
        if (!active_.has_value()) {
            active_ = operation;
            buildingInventoryRefreshed_ = false;
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
        buildingInventoryRefreshed_ = false;
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
        buildingInventoryRefreshed_ = false;
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

    /** @return 当前建造会话是否仍需在 Setup 后通知一次库存内容更新。 */
    [[nodiscard]] auto building_inventory_refresh_needed(
        const std::uint64_t generation) const noexcept -> bool {
        return generation == generation_ && active_ == ResourceOperation::building &&
               !buildingInventoryRefreshed_;
    }

    /**
     * @brief 标记当前建造会话已经成功发送库存内容更新。
     * @retval true 首次完成当前有效建造会话的通知。
     * @retval false 世界或会话不匹配，或该会话已经通知。
     */
    [[nodiscard]] auto complete_building_inventory_refresh(const std::uint64_t generation) noexcept
        -> bool {
        if (!building_inventory_refresh_needed(generation)) {
            return false;
        }
        buildingInventoryRefreshed_ = true;
        return true;
    }

    /** @brief 清空所有代次和所有权状态。 */
    auto reset() noexcept -> void {
        generation_ = 0;
        active_.reset();
        buildingInventoryRefreshed_ = false;
    }

private:
    std::uint64_t generation_{};
    std::optional<ResourceOperation> active_;
    bool buildingInventoryRefreshed_{};
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
