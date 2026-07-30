#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include <base_resource_sharing/persistent_union.hpp>
#include <base_resource_sharing/resource_pool.hpp>

namespace base_resource_sharing {
struct BaseResourceSharingSnapshot {
    /** @brief 用户是否启用了本进程资源共享。 */
    bool enabled{};
    /** @brief 当前世界是否允许访问 Unreal 对象。 */
    bool worldAccessible{};
    /** @brief 当前 LoadMap 世界代次。 */
    std::uint64_t worldGeneration{};
    /** @brief 当前公会据点数量。 */
    std::size_t baseCount{};
    /** @brief 已加载的原生普通箱子数量。 */
    std::size_t containerCount{};
    /** @brief 已登记但 ConcreteModel 尚未加载的普通箱子数量。 */
    std::size_t pendingContainerCount{};
    /** @brief 持久公会仓储图当前生命周期阶段。 */
    PersistentUnionPhase persistentPhase{PersistentUnionPhase::off};
    /** @brief 已由本 Mod 验证并记账的原生登记边数。 */
    std::size_t appliedEdgeCount{};
    /** @brief 当前帧预算队列中尚未处理的新增与删除边数。 */
    std::size_t pendingEdgeCount{};
    /** @brief 制作、建造和修理三项能力状态。 */
    std::array<CapabilityState, 3> capabilities{};
    /** @brief 当前向 Palworld 暴露共享材料的原生消费面。 */
    ResourceConsumerSurface consumerSurface{ResourceConsumerSurface::none};
    /** @brief 最近一次目录发现耗时。 */
    double lastCatalogMilliseconds{};
    /** @brief 最近一次成功目录发现耗时。 */
    double lastSuccessfulCatalogMilliseconds{};
    /** @brief 本世界目录发现最大耗时。 */
    double maximumCatalogMilliseconds{};
    /** @brief 本世界目录发现总次数。 */
    std::size_t catalogAttemptCount{};
    /** @brief 是否因不可验证的写入或恢复错误而安全停用。 */
    bool safetyDisabled{};
    /** @brief 可直接显示在 GUI 中的中文状态文本。 */
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
