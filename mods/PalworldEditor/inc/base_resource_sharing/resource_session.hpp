#pragma once

#include <algorithm>
#include <cstdint>

namespace base_resource_sharing {
inline constexpr float kCatalogRetrySeconds = 1.0F;
inline constexpr float kCatalogReconcileSeconds = 8.0F;
inline constexpr float kCraftingLeaseIdleSeconds = 1.5F;

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

    [[nodiscard]] auto advance(const float deltaSeconds, const std::uint64_t generation) noexcept
        -> bool {
        if (generation != generation_ || inFlight_) {
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

class ResourceUnionLeaseState {
public:
    auto begin_world(const std::uint64_t generation) noexcept -> void {
        generation_ = generation;
        buildingActive_ = false;
        craftingActive_ = false;
        craftingIdleSeconds_ = 0.0F;
    }

    [[nodiscard]] auto acquire_building(const std::uint64_t generation) noexcept -> bool {
        if (generation != generation_ || buildingActive_) {
            return false;
        }
        buildingActive_ = true;
        return true;
    }

    [[nodiscard]] auto release_building(const std::uint64_t generation) noexcept -> bool {
        if (generation != generation_ || !buildingActive_) {
            return false;
        }
        buildingActive_ = false;
        return true;
    }

    [[nodiscard]] auto touch_crafting(const std::uint64_t generation) noexcept -> bool {
        if (generation != generation_) {
            return false;
        }
        const bool changed = !craftingActive_;
        craftingActive_ = true;
        craftingIdleSeconds_ = 0.0F;
        return changed;
    }

    [[nodiscard]] auto advance(const float deltaSeconds, const std::uint64_t generation) noexcept
        -> bool {
        if (generation != generation_ || !craftingActive_) {
            return false;
        }
        craftingIdleSeconds_ += std::max(deltaSeconds, 0.0F);
        if (craftingIdleSeconds_ < kCraftingLeaseIdleSeconds) {
            return false;
        }

        craftingActive_ = false;
        craftingIdleSeconds_ = 0.0F;
        return !buildingActive_;
    }

    [[nodiscard]] auto desired(const std::uint64_t generation) const noexcept -> bool {
        return generation == generation_ && (buildingActive_ || craftingActive_);
    }

    auto reset() noexcept -> void {
        generation_ = 0;
        buildingActive_ = false;
        craftingActive_ = false;
        craftingIdleSeconds_ = 0.0F;
    }

private:
    std::uint64_t generation_{};
    bool buildingActive_{};
    bool craftingActive_{};
    float craftingIdleSeconds_{};
};
}  // namespace base_resource_sharing
