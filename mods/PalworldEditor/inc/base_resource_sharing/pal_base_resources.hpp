#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include <base_resource_sharing/resource_pool.hpp>

namespace base_resource_sharing {
struct BaseResourceSharingSnapshot {
    bool enabled{};
    bool worldAccessible{};
    std::uint64_t worldGeneration{};
    std::size_t baseCount{};
    std::size_t containerCount{};
    std::array<CapabilityState, 3> capabilities{};
    std::optional<ResourceOperation> foregroundOperation;
    ResourceConsumerSurface consumerSurface{ResourceConsumerSurface::none};
    std::optional<GuidKey> currentBaseId;
    double lastCatalogMilliseconds{};
    double lastUnionMilliseconds{};
    bool safetyDisabled{};
    std::string status;
};

class PalBaseResourceBridge final {
public:
    PalBaseResourceBridge();
    ~PalBaseResourceBridge();
    PalBaseResourceBridge(const PalBaseResourceBridge&) = delete;
    auto operator=(const PalBaseResourceBridge&) -> PalBaseResourceBridge& = delete;
    PalBaseResourceBridge(PalBaseResourceBridge&&) noexcept;
    auto operator=(PalBaseResourceBridge&&) noexcept -> PalBaseResourceBridge&;

    auto set_enabled(bool enabled) -> void;
    auto on_world_begin(std::uint64_t generation) -> void;
    auto on_world_ready(std::uint64_t generation) -> void;
    auto tick(float deltaSeconds) -> void;
    auto ensure_hooks_registered() -> void;
    auto shutdown_hooks() -> void;
    [[nodiscard]] auto snapshot() const -> BaseResourceSharingSnapshot;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace base_resource_sharing
