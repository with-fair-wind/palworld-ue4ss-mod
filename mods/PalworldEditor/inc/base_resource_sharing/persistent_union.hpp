#pragma once

#include <algorithm>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <base_resource_sharing/resource_pool.hpp>

namespace base_resource_sharing {
/** @brief 一个可登记到据点仓储模块的普通箱子。 */
struct PersistentStorageContainer {
    /** @brief Palworld 物品容器 GUID。 */
    GuidKey containerId;
    /** @brief 用于重新解析 ConcreteModel 的地图物体 GUID。 */
    GuidKey ownerMapObjectId;

    auto operator<=>(const PersistentStorageContainer&) const = default;
};

/** @brief 一个据点的仓储模块及其原生普通箱子。 */
struct PersistentStorageModule {
    /** @brief 仓储模块所属据点 GUID。 */
    GuidKey baseId;
    /** @brief 跨帧重新解析仓储模块所需的完整对象名。 */
    std::wstring objectFullName;
    /** @brief 该模块原生登记且当前已加载的普通箱子。 */
    std::vector<PersistentStorageContainer> containers;
};

/** @brief 一条由本 Mod 建立的跨据点原生仓储登记关系。 */
struct PersistentUnionEdge {
    /** @brief 接收远端容器登记的目标据点。 */
    GuidKey targetBaseId;
    /** @brief 目标仓储模块完整对象名。 */
    std::wstring targetModuleFullName;
    /** @brief 提供普通箱子的来源据点。 */
    GuidKey sourceBaseId;
    /** @brief 被登记的物品容器 GUID。 */
    GuidKey containerId;
    /** @brief 被登记容器对应的地图物体 GUID。 */
    GuidKey ownerMapObjectId;

    auto operator<=>(const PersistentUnionEdge&) const = default;
};

/** @brief 当前目录所要求的全部跨据点登记边。 */
struct PersistentUnionPlan {
    /** @brief 排序并去重后的期望跨据点登记边。 */
    std::vector<PersistentUnionEdge> edges;
    /** @brief 无法生成安全计划时的中文错误。 */
    std::string error;
};

/** @brief 已登记边和期望边之间需要执行的最小差量。 */
struct PersistentUnionDiff {
    /** @brief 当前账本缺少、需要新增的边。 */
    std::vector<PersistentUnionEdge> added;
    /** @brief 当前账本多出、需要注销的边。 */
    std::vector<PersistentUnionEdge> removed;
};

/**
 * @brief 从目录快照中剔除本 Mod 已登记到目标模块的容器，只保留各据点原生来源。
 */
[[nodiscard]] inline auto remove_applied_target_edges(
    std::vector<PersistentStorageModule> modules,
    const std::span<const PersistentUnionEdge> applied) -> std::vector<PersistentStorageModule> {
    for (auto& module : modules) {
        std::erase_if(module.containers, [&](const auto& container) {
            return std::ranges::any_of(applied, [&](const auto& edge) {
                return edge.targetBaseId == module.baseId &&
                       edge.targetModuleFullName == module.objectFullName &&
                       edge.containerId == container.containerId;
            });
        });
    }
    return modules;
}

/** @brief 一次原生登记或注销调用相对于调用前序列产生的精确结果。 */
enum class PersistentEdgeMutation : std::uint8_t {
    unchanged,
    added,
    removed,
    invalid,
};

/** @return 原生登记前后目标容器出现次数对应的安全记账结果。 */
[[nodiscard]] constexpr auto classify_persistent_edge_add(const std::size_t beforeCount,
                                                          const std::size_t afterCount) noexcept
    -> PersistentEdgeMutation {
    if (beforeCount == 1 && afterCount == 1) {
        return PersistentEdgeMutation::unchanged;
    }
    if (beforeCount == 0 && afterCount == 1) {
        return PersistentEdgeMutation::added;
    }
    return PersistentEdgeMutation::invalid;
}

/** @return 原生注销前后目标容器出现次数对应的安全恢复结果。 */
[[nodiscard]] constexpr auto classify_persistent_edge_remove(const std::size_t beforeCount,
                                                             const std::size_t afterCount) noexcept
    -> PersistentEdgeMutation {
    if (beforeCount == 0 && afterCount == 0) {
        return PersistentEdgeMutation::unchanged;
    }
    if (beforeCount == 1 && afterCount == 0) {
        return PersistentEdgeMutation::removed;
    }
    return PersistentEdgeMutation::invalid;
}

namespace detail {
[[nodiscard]] inline auto normalized_edges(const std::span<const PersistentUnionEdge> edges)
    -> std::vector<PersistentUnionEdge> {
    std::vector<PersistentUnionEdge> result{edges.begin(), edges.end()};
    std::ranges::sort(result);
    const auto duplicate = std::ranges::unique(result);
    result.erase(duplicate.begin(), duplicate.end());
    return result;
}
}  // namespace detail

/**
 * @brief 为每个有效据点模块建立指向所有其他据点普通箱子的稳定登记计划。
 * @details 同据点容器、无效标识和重复容器不会形成登记边；单据点和空目录是有效空计划。
 */
[[nodiscard]] inline auto make_persistent_union_plan(
    const std::span<const PersistentStorageModule> modules) -> PersistentUnionPlan {
    PersistentUnionPlan result;
    std::vector<PersistentStorageModule> validModules;
    validModules.reserve(modules.size());
    for (const auto& module : modules) {
        if (!module.baseId.valid() || module.objectFullName.empty()) {
            continue;
        }
        auto copy = module;
        std::erase_if(copy.containers, [](const auto& container) {
            return !container.containerId.valid() || !container.ownerMapObjectId.valid();
        });
        std::ranges::sort(copy.containers);
        const auto duplicate =
            std::ranges::unique(copy.containers, {}, &PersistentStorageContainer::containerId);
        copy.containers.erase(duplicate.begin(), duplicate.end());
        validModules.push_back(std::move(copy));
    }
    std::ranges::sort(validModules, [](const auto& left, const auto& right) {
        if (left.baseId != right.baseId) {
            return left.baseId < right.baseId;
        }
        return left.objectFullName < right.objectFullName;
    });
    if (validModules.empty()) {
        result.error = "未发现可建立持久联合的同公会据点仓储模块。";
        return result;
    }

    for (const auto& target : validModules) {
        for (const auto& source : validModules) {
            if (target.baseId == source.baseId) {
                continue;
            }
            for (const auto& container : source.containers) {
                result.edges.push_back({
                    .targetBaseId = target.baseId,
                    .targetModuleFullName = target.objectFullName,
                    .sourceBaseId = source.baseId,
                    .containerId = container.containerId,
                    .ownerMapObjectId = container.ownerMapObjectId,
                });
            }
        }
    }
    result.edges = detail::normalized_edges(result.edges);
    return result;
}

/** @return 从已登记集合转换到期望集合所需的新增边和删除边。 */
[[nodiscard]] inline auto diff_persistent_union(const std::span<const PersistentUnionEdge> desired,
                                                const std::span<const PersistentUnionEdge> applied)
    -> PersistentUnionDiff {
    const auto normalizedDesired = detail::normalized_edges(desired);
    const auto normalizedApplied = detail::normalized_edges(applied);
    PersistentUnionDiff result;
    for (const auto& edge : normalizedDesired) {
        if (!std::ranges::binary_search(normalizedApplied, edge)) {
            result.added.push_back(edge);
        }
    }
    for (const auto& edge : normalizedApplied) {
        if (!std::ranges::binary_search(normalizedDesired, edge)) {
            result.removed.push_back(edge);
        }
    }
    return result;
}

/** @brief 一个世界代次内持久联合的生命周期阶段。 */
enum class PersistentUnionPhase : std::uint8_t {
    off,
    initializing,
    ready,
    reconciling,
    restoring,
    failed,
};

/** @brief 不保存 Unreal 对象的持久联合生命周期状态机。 */
class PersistentUnionLifecycle {
public:
    auto begin_world(const std::uint64_t generation) noexcept -> void {
        generation_ = generation;
        phase_ = PersistentUnionPhase::off;
    }

    [[nodiscard]] auto request_enable(const std::uint64_t generation) noexcept -> bool {
        if (generation != generation_ || phase_ != PersistentUnionPhase::off) {
            return false;
        }
        phase_ = PersistentUnionPhase::initializing;
        return true;
    }

    [[nodiscard]] auto complete_apply(const std::uint64_t generation) noexcept -> bool {
        if (generation != generation_ || (phase_ != PersistentUnionPhase::initializing &&
                                          phase_ != PersistentUnionPhase::reconciling)) {
            return false;
        }
        phase_ = PersistentUnionPhase::ready;
        return true;
    }

    [[nodiscard]] auto invalidate(const std::uint64_t generation) noexcept -> bool {
        if (generation != generation_ || phase_ != PersistentUnionPhase::ready) {
            return false;
        }
        phase_ = PersistentUnionPhase::reconciling;
        return true;
    }

    [[nodiscard]] auto request_disable(const std::uint64_t generation) noexcept -> bool {
        if (generation != generation_ || phase_ == PersistentUnionPhase::off ||
            phase_ == PersistentUnionPhase::restoring) {
            return false;
        }
        phase_ = PersistentUnionPhase::restoring;
        return true;
    }

    [[nodiscard]] auto complete_restore(const std::uint64_t generation) noexcept -> bool {
        if (generation != generation_ || phase_ != PersistentUnionPhase::restoring) {
            return false;
        }
        phase_ = PersistentUnionPhase::off;
        return true;
    }

    auto fail(const std::uint64_t generation) noexcept -> void {
        if (generation == generation_) {
            phase_ = PersistentUnionPhase::failed;
        }
    }

    [[nodiscard]] auto phase(const std::uint64_t generation) const noexcept
        -> PersistentUnionPhase {
        return generation == generation_ ? phase_ : PersistentUnionPhase::off;
    }

private:
    std::uint64_t generation_{};
    PersistentUnionPhase phase_{PersistentUnionPhase::off};
};

/** @brief 只记录已经验证为本 Mod 新增的登记边。 */
class PersistentUnionLedger {
public:
    [[nodiscard]] auto record(const PersistentUnionEdge& edge) -> bool {
        const auto position = std::ranges::lower_bound(edges_, edge);
        if (position != edges_.end() && *position == edge) {
            return false;
        }
        edges_.insert(position, edge);
        return true;
    }

    [[nodiscard]] auto erase(const PersistentUnionEdge& edge) -> bool {
        const auto position = std::ranges::lower_bound(edges_, edge);
        if (position == edges_.end() || *position != edge) {
            return false;
        }
        edges_.erase(position);
        return true;
    }

    [[nodiscard]] auto contains(const PersistentUnionEdge& edge) const noexcept -> bool {
        return std::ranges::binary_search(edges_, edge);
    }

    [[nodiscard]] auto edges() const noexcept -> std::span<const PersistentUnionEdge> {
        return edges_;
    }

    [[nodiscard]] auto empty() const noexcept -> bool {
        return edges_.empty();
    }

    auto clear() noexcept -> void {
        edges_.clear();
    }

private:
    std::vector<PersistentUnionEdge> edges_;
};

/** @brief 单个 EngineTick 的持久联合工作预算。 */
class PersistentUnionWorkBudget {
public:
    static constexpr std::size_t MaxOperationsPerFrame{4};
    static constexpr std::chrono::microseconds MaxElapsedPerFrame{500};

    [[nodiscard]] auto can_process(const std::chrono::microseconds elapsed) const noexcept -> bool {
        return processed_ < MaxOperationsPerFrame && elapsed < MaxElapsedPerFrame;
    }

    auto record_operation() noexcept -> void {
        ++processed_;
    }

    auto reset() noexcept -> void {
        processed_ = 0;
    }

private:
    std::size_t processed_{};
};
}  // namespace base_resource_sharing
