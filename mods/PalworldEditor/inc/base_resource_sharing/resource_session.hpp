#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

#include <base_resource_sharing/resource_pool.hpp>

namespace base_resource_sharing {
inline constexpr float kCatalogRetrySeconds = 1.0F;
inline constexpr float kCatalogReconcileSeconds = 8.0F;
inline constexpr float kCraftingLeaseIdleSeconds = 1.5F;

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

enum class SessionEndPolicy : std::uint8_t { explicitRelease, idleTimeout };

struct OperationSessionPolicy {
    ResourceOperation operation;
    SessionEndPolicy endPolicy;
    float idleSeconds{};
};

inline constexpr std::array kOperationSessionPolicies{
    OperationSessionPolicy{ResourceOperation::crafting, SessionEndPolicy::idleTimeout,
                           kCraftingLeaseIdleSeconds},
    OperationSessionPolicy{ResourceOperation::building, SessionEndPolicy::explicitRelease, 0.0F},
    OperationSessionPolicy{ResourceOperation::repair, SessionEndPolicy::explicitRelease, 0.0F},
};

struct SessionTransition {
    bool unionBecameDesired{};
    bool unionBecameIdle{};
};

class MaterialOperationSessions {
public:
    auto begin_world(const std::uint64_t generation) noexcept -> void {
        generation_ = generation;
        states_ = {};
    }

    [[nodiscard]] auto acquire(const ResourceOperation operation,
                               const std::uint64_t generation) noexcept -> SessionTransition {
        if (generation != generation_) {
            return {};
        }
        const bool wasDesired = desired(generation);
        auto& state = states_[operation_index(operation)];
        state.active = true;
        state.idleSeconds = 0.0F;
        return {.unionBecameDesired = !wasDesired};
    }

    [[nodiscard]] auto touch(const ResourceOperation operation,
                             const std::uint64_t generation) noexcept -> bool {
        if (generation != generation_) {
            return false;
        }
        auto& state = states_[operation_index(operation)];
        if (!state.active) {
            return false;
        }
        state.idleSeconds = 0.0F;
        return true;
    }

    [[nodiscard]] auto release(const ResourceOperation operation,
                               const std::uint64_t generation) noexcept -> SessionTransition {
        if (generation != generation_) {
            return {};
        }
        const bool wasDesired = desired(generation);
        auto& state = states_[operation_index(operation)];
        state = {};
        return {.unionBecameIdle = wasDesired && !desired(generation)};
    }

    [[nodiscard]] auto cancel_all(const std::uint64_t generation) noexcept -> SessionTransition {
        if (generation != generation_) {
            return {};
        }
        const bool wasDesired = desired(generation);
        states_ = {};
        return {.unionBecameIdle = wasDesired};
    }

    [[nodiscard]] auto advance(const float deltaSeconds, const std::uint64_t generation) noexcept
        -> SessionTransition {
        if (generation != generation_) {
            return {};
        }
        const bool wasDesired = desired(generation);
        const auto elapsed = std::max(deltaSeconds, 0.0F);
        for (const auto& policy : kOperationSessionPolicies) {
            auto& state = states_[operation_index(policy.operation)];
            if (!state.active || policy.endPolicy != SessionEndPolicy::idleTimeout) {
                continue;
            }
            state.idleSeconds += elapsed;
            if (state.idleSeconds >= policy.idleSeconds) {
                state = {};
            }
        }
        return {.unionBecameIdle = wasDesired && !desired(generation)};
    }

    [[nodiscard]] auto desired(const std::uint64_t generation) const noexcept -> bool {
        return generation == generation_ &&
               std::ranges::any_of(states_, [](const auto& state) { return state.active; });
    }

    [[nodiscard]] auto active(const ResourceOperation operation,
                              const std::uint64_t generation) const noexcept -> bool {
        return generation == generation_ && states_[operation_index(operation)].active;
    }

    [[nodiscard]] auto required_targets(const std::uint64_t generation) const noexcept
        -> UnionTargets {
        if (generation != generation_) {
            return {};
        }
        UnionTargets result;
        for (const auto operation : {ResourceOperation::crafting, ResourceOperation::building,
                                     ResourceOperation::repair}) {
            if (active(operation, generation)) {
                result = combine_union_targets(result, union_targets_for_operation(operation));
            }
        }
        return result;
    }

    auto reset() noexcept -> void {
        generation_ = 0;
        states_ = {};
    }

private:
    struct OperationState {
        bool active{};
        float idleSeconds{};
    };

    std::uint64_t generation_{};
    std::array<OperationState, 3> states_{};
};
}  // namespace base_resource_sharing
