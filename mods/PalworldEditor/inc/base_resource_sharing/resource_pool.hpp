#pragma once

#include <algorithm>
#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace base_resource_sharing {
struct GuidKey {
    std::array<std::uint32_t, 4> words{};

    [[nodiscard]] auto valid() const noexcept -> bool {
        return std::ranges::any_of(words, [](const auto word) { return word != 0; });
    }

    auto operator<=>(const GuidKey&) const = default;
};

enum class ContainerKind : std::uint8_t { normal, food, player, other };

struct ContainerDescriptor {
    GuidKey baseId;
    GuidKey groupId;
    GuidKey containerId;
    ContainerKind kind{ContainerKind::other};
    bool currentBase{};
};

struct ResourceUnionPlan {
    std::vector<ContainerDescriptor> ordered;
    std::size_t baseCount{};
    std::string error;
};

[[nodiscard]] inline auto make_resource_union_plan(
    const std::span<const ContainerDescriptor> containers, const GuidKey& currentGuild)
    -> ResourceUnionPlan {
    ResourceUnionPlan result;
    if (!currentGuild.valid()) {
        result.error = "当前玩家的公会标识无效。";
        return result;
    }

    for (const auto& descriptor : containers) {
        if (descriptor.kind == ContainerKind::normal && descriptor.baseId.valid() &&
            descriptor.containerId.valid() && descriptor.groupId == currentGuild) {
            result.ordered.push_back(descriptor);
        }
    }

    std::ranges::sort(result.ordered, [](const auto& left, const auto& right) {
        if (left.currentBase != right.currentBase) {
            return left.currentBase;
        }
        if (left.baseId != right.baseId) {
            return left.baseId < right.baseId;
        }
        return left.containerId < right.containerId;
    });
    const auto duplicate = std::ranges::unique(
        result.ordered, {}, [](const auto& descriptor) { return descriptor.containerId; });
    result.ordered.erase(duplicate.begin(), duplicate.end());

    std::vector<GuidKey> bases;
    for (const auto& descriptor : result.ordered) {
        if (std::ranges::find(bases, descriptor.baseId) == bases.end()) {
            bases.push_back(descriptor.baseId);
        }
    }
    result.baseCount = bases.size();
    if (result.ordered.empty()) {
        result.error = "未发现当前公会已加载的普通据点资源容器。";
    }
    return result;
}

enum class ResourceOperation : std::uint8_t { crafting, building, repair };

[[nodiscard]] constexpr auto operation_index(const ResourceOperation operation) noexcept
    -> std::size_t {
    return static_cast<std::size_t>(operation);
}

/** @brief 一次材料操作唯一允许注入的 Palworld 消费面。 */
enum class ResourceConsumerSurface : std::uint8_t {
    none,
    playerHelper,
    currentBaseModule,
};

/** @brief 前台材料操作及其唯一消费面；不会组合多个入口。 */
struct ResourceExposurePlan {
    ResourceOperation operation{ResourceOperation::repair};
    ResourceConsumerSurface surface{ResourceConsumerSurface::none};
    std::optional<GuidKey> targetBaseId;

    auto operator<=>(const ResourceExposurePlan&) const = default;
};

/** @brief 为制作或建造选择唯一消费面；无法确认当前据点时拒绝扩展。 */
[[nodiscard]] inline auto make_exposure_plan(
    const ResourceOperation operation,
    const std::optional<GuidKey> currentBaseId = std::nullopt) noexcept -> ResourceExposurePlan {
    switch (operation) {
        case ResourceOperation::crafting:
            if (currentBaseId.has_value() && currentBaseId->valid()) {
                return {
                    .operation = operation,
                    .surface = ResourceConsumerSurface::playerHelper,
                    .targetBaseId = currentBaseId,
                };
            }
            return {.operation = operation};
        case ResourceOperation::building:
            if (currentBaseId.has_value() && currentBaseId->valid()) {
                return {
                    .operation = operation,
                    .surface = ResourceConsumerSurface::currentBaseModule,
                    .targetBaseId = currentBaseId,
                };
            }
            return {.operation = operation};
        case ResourceOperation::repair:
            return {
                .operation = operation,
            };
    }
    return {.operation = operation};
}

/**
 * @brief 选择其他据点的唯一容器，避免当前据点同时经原版入口和共享入口重复统计。
 */
[[nodiscard]] inline auto select_shared_container_ids(
    const std::span<const ContainerDescriptor> containers, const GuidKey& currentBaseId)
    -> std::vector<GuidKey> {
    std::vector<GuidKey> result;
    result.reserve(containers.size());
    for (const auto& container : containers) {
        if (container.baseId != currentBaseId &&
            std::ranges::find(result, container.containerId) == result.end()) {
            result.push_back(container.containerId);
        }
    }
    return result;
}

enum class BuildingInventoryRefreshTarget : std::uint8_t {
    none,
    buildModel,
};

/** @return 建造联合建立后唯一允许发送库存更新的原生目标。 */
[[nodiscard]] constexpr auto select_building_inventory_refresh_target(
    const bool unionActive, const ResourceExposurePlan& exposure) noexcept
    -> BuildingInventoryRefreshTarget {
    if (unionActive && exposure.operation == ResourceOperation::building &&
        exposure.surface == ResourceConsumerSurface::currentBaseModule) {
        return BuildingInventoryRefreshTarget::buildModel;
    }
    return BuildingInventoryRefreshTarget::none;
}

enum class BuildingMenuBoundaryAction : std::uint8_t { acquire, reuse, replace };

/** @return 新建筑菜单边界应获取、复用还是替换当前联合。 */
[[nodiscard]] constexpr auto decide_building_menu_boundary(
    const bool unionActive, const std::uint64_t unionGeneration,
    const std::uint64_t currentGeneration, const ResourceExposurePlan& exposure,
    const std::optional<GuidKey> observedBase) noexcept -> BuildingMenuBoundaryAction {
    if (!unionActive) {
        return BuildingMenuBoundaryAction::acquire;
    }
    const bool sameBase = unionGeneration == currentGeneration && observedBase.has_value() &&
                          exposure.operation == ResourceOperation::building &&
                          exposure.surface == ResourceConsumerSurface::currentBaseModule &&
                          exposure.targetBaseId == observedBase;
    return sameBase ? BuildingMenuBoundaryAction::reuse : BuildingMenuBoundaryAction::replace;
}

/** @return 当前 Setup post 是否应向 BuildModel 发送一次原生库存更新。 */
[[nodiscard]] constexpr auto should_refresh_building_inventory(
    const bool refreshNeeded, const bool unionActive, const std::uint64_t unionGeneration,
    const std::uint64_t currentGeneration, const ResourceExposurePlan& exposure,
    const bool hasLedgerEntry, const bool helperArray) noexcept -> bool {
    return refreshNeeded && unionGeneration == currentGeneration && hasLedgerEntry &&
           !helperArray &&
           select_building_inventory_refresh_target(unionActive, exposure) ==
               BuildingInventoryRefreshTarget::buildModel;
}

[[nodiscard]] constexpr auto resource_hooks_required(const bool enabled,
                                                     const bool worldAccessible) noexcept -> bool {
    return enabled && worldAccessible;
}

class SnapshotDirtyFlag {
public:
    auto mark() noexcept -> void {
        dirty_ = true;
    }

    [[nodiscard]] auto consume() noexcept -> bool {
        return std::exchange(dirty_, false);
    }

private:
    bool dirty_{true};
};

struct CapabilityState {
    bool previewReady{};
    bool consumeReady{};
    std::string error;

    [[nodiscard]] auto available() const noexcept -> bool {
        return previewReady && consumeReady;
    }
};

class RuntimeState {
public:
    auto set_preference(const bool enabled) noexcept -> void {
        enabled_ = enabled;
    }

    auto begin_world_transition(const std::uint64_t generation) -> void {
        generation_ = generation;
        accessible_ = false;
        capabilities_ = {};
    }

    auto finish_world_transition(const std::uint64_t generation) -> void {
        generation_ = generation;
        accessible_ = true;
        capabilities_ = {};
    }

    auto set_capability(const ResourceOperation operation, CapabilityState capability) -> void {
        capabilities_[operation_index(operation)] = std::move(capability);
    }

    [[nodiscard]] auto can_extend(const ResourceOperation operation,
                                  const std::uint64_t generation) const -> bool {
        return enabled_ && accessible_ && generation == generation_ &&
               capabilities_[operation_index(operation)].available();
    }

    [[nodiscard]] auto generation() const noexcept -> std::uint64_t {
        return generation_;
    }

    [[nodiscard]] auto enabled() const noexcept -> bool {
        return enabled_;
    }

    [[nodiscard]] auto accessible() const noexcept -> bool {
        return accessible_;
    }

    [[nodiscard]] auto capability(const ResourceOperation operation) const
        -> const CapabilityState& {
        return capabilities_[operation_index(operation)];
    }

private:
    bool enabled_{};
    bool accessible_{};
    std::uint64_t generation_{};
    std::array<CapabilityState, 3> capabilities_{};
};

struct InjectionRemovalPlan {
    std::vector<GuidKey> kept;
    bool complete{};
};

[[nodiscard]] inline auto remove_recorded_injections(const std::span<const GuidKey> current,
                                                     const std::span<const GuidKey> original,
                                                     const std::span<const GuidKey> injected)
    -> InjectionRemovalPlan {
    std::map<GuidKey, std::size_t> originalCounts;
    std::map<GuidKey, std::size_t> injectedCounts;
    for (const auto& id : original) {
        ++originalCounts[id];
    }
    for (const auto& id : injected) {
        ++injectedCounts[id];
    }

    std::map<GuidKey, std::size_t> encounteredCounts;
    std::map<GuidKey, std::size_t> removedCounts;
    InjectionRemovalPlan result;
    result.kept.reserve(current.size());
    for (const auto& id : current) {
        const auto encountered = ++encounteredCounts[id];
        if (encountered <= originalCounts[id]) {
            result.kept.push_back(id);
            continue;
        }
        if (removedCounts[id] < injectedCounts[id]) {
            ++removedCounts[id];
            continue;
        }
        result.kept.push_back(id);
    }

    result.complete = std::ranges::all_of(injectedCounts, [&](const auto& entry) {
        const auto removed = removedCounts.find(entry.first);
        return removed != removedCounts.end() && removed->second == entry.second;
    });
    return result;
}

[[nodiscard]] inline auto missing_union_tail(const std::span<const GuidKey> existing,
                                             const std::span<const GuidKey> globalPlan)
    -> std::vector<GuidKey> {
    std::vector<GuidKey> missing;
    for (const auto& id : globalPlan) {
        if (std::ranges::find(existing, id) == existing.end() &&
            std::ranges::find(missing, id) == missing.end()) {
            missing.push_back(id);
        }
    }
    return missing;
}

/** @brief 注入后的序列验证结果。 */
enum class SequenceValidationStatus : std::uint8_t {
    valid,
    originalPrefixChanged,
    injectedCountMismatch,
    duplicateInjectedId,
    unexpectedTail,
};

/** @brief 精确指出序列验证失败类别及首个相关容器。 */
struct SequenceValidationResult {
    SequenceValidationStatus status{SequenceValidationStatus::valid};
    std::optional<GuidKey> offendingId;

    [[nodiscard]] explicit operator bool() const noexcept {
        return status == SequenceValidationStatus::valid;
    }
};

/**
 * @brief 验证当前序列严格等于原序列加按记录顺序追加的一份唯一注入。
 * @details 原序列中的既有重复允许保留；注入不得重复原序列或其他注入项。
 */
[[nodiscard]] inline auto validate_applied_sequence(const std::span<const GuidKey> original,
                                                    const std::span<const GuidKey> injected,
                                                    const std::span<const GuidKey> current) noexcept
    -> SequenceValidationResult {
    for (std::size_t index = 0; index < injected.size(); ++index) {
        const auto id = injected[index];
        if (std::ranges::find(original, id) != original.end() ||
            std::ranges::find(injected.first(index), id) != injected.first(index).end()) {
            return {
                .status = SequenceValidationStatus::duplicateInjectedId,
                .offendingId = id,
            };
        }
    }

    if (current.size() < original.size()) {
        return {.status = SequenceValidationStatus::originalPrefixChanged};
    }
    for (std::size_t index = 0; index < original.size(); ++index) {
        if (current[index] != original[index]) {
            return {
                .status = SequenceValidationStatus::originalPrefixChanged,
                .offendingId = current[index],
            };
        }
    }

    const auto expectedSize = original.size() + injected.size();
    if (current.size() < expectedSize) {
        return {.status = SequenceValidationStatus::injectedCountMismatch};
    }
    for (std::size_t index = 0; index < injected.size(); ++index) {
        if (current[original.size() + index] != injected[index]) {
            return {
                .status = SequenceValidationStatus::injectedCountMismatch,
                .offendingId = current[original.size() + index],
            };
        }
    }
    if (current.size() > expectedSize) {
        return {
            .status = SequenceValidationStatus::unexpectedTail,
            .offendingId = current[expectedSize],
        };
    }
    return {};
}

struct BaseResourceSharingStatus {
    bool enabled{};
    bool worldAccessible{true};
    bool detectingCapabilities{};
    std::size_t baseCount{};
    std::size_t containerCount{};
    std::size_t pendingContainerCount{};
    bool craftingAvailable{};
    bool buildingAvailable{};
    bool repairAvailable{};
    std::string craftingError;
    std::string buildingError;
    std::string repairError;
    std::string runtimeError;
};

[[nodiscard]] inline auto format_status(const BaseResourceSharingStatus& status) -> std::string {
    if (!status.enabled) {
        return "据点资源共享已关闭。";
    }
    if (!status.worldAccessible) {
        return "正在等待可访问的游戏世界。";
    }
    if (status.detectingCapabilities) {
        std::string text = "正在检测 Palworld 资源接口。";
        if (!status.craftingError.empty()) {
            text += "\n制作：" + status.craftingError;
        }
        if (!status.buildingError.empty()) {
            text += "\n建造：" + status.buildingError;
        }
        return text;
    }

    std::string text = "已发现 " + std::to_string(status.baseCount) + " 个据点、" +
                       std::to_string(status.containerCount) + " 个资源容器。\n";
    if (status.pendingContainerCount > 0) {
        text += std::to_string(status.pendingContainerCount) +
                " 个容器暂未加载，已排除并安排低频重试。\n";
    }
    text += status.craftingAvailable ? "制作：可用" : "制作：不可用";
    text += status.buildingAvailable ? "；建造：可用" : "；建造：不可用";
    text += status.repairAvailable ? "；修理：可用" : "；修理：不可用";
    if (!status.craftingAvailable && !status.craftingError.empty()) {
        text += "\n制作：" + status.craftingError;
    }
    if (!status.buildingAvailable && !status.buildingError.empty()) {
        text += "\n建造：" + status.buildingError;
    }
    if (!status.repairAvailable && !status.repairError.empty()) {
        text += "\n" + status.repairError;
    }
    if (!status.runtimeError.empty()) {
        text += "\n错误：" + status.runtimeError;
    }
    return text;
}
}  // namespace base_resource_sharing
