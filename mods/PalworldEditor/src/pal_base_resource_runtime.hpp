#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <base_resource_sharing/persistent_union.hpp>
#include <base_resource_sharing/resource_pool.hpp>

namespace RC::Unreal {
class UObject;
}

namespace base_resource_sharing::detail {
struct CatalogContainer {
    GuidKey containerId;
    GuidKey ownerMapObjectId;
};

struct CatalogModule {
    GuidKey baseId;
    std::wstring objectFullName;
    std::vector<CatalogContainer> containers;
};

struct ResourceCatalogSnapshot {
    std::uint64_t generation{};
    GuidKey guildId;
    bool initialized{};
    std::size_t sameGuildBaseCount{};
    std::size_t registeredContainerCount{};
    std::size_t pendingContainerCount{};
    ResourceUnionPlan plan;
    std::vector<CatalogModule> modules;
    std::string error;
};

struct UnionLedgerEntry {
    std::wstring objectFullName;
    bool helperArray{};
    std::vector<GuidKey> original;
    std::vector<GuidKey> injected;
};

struct LiveUnion {
    std::uint64_t generation{};
    GuidKey guildId;
    ResourceExposurePlan exposure;
    std::optional<UnionLedgerEntry> entry;
    bool active{};
};

/** @brief 一次原生持久登记边操作的可验证结果。 */
struct PersistentEdgeMutationResult {
    /** @brief 原生调用可验证出的边变化。 */
    PersistentEdgeMutation mutation{PersistentEdgeMutation::invalid};
    /** @brief 失败或无法完整验证时的中文错误。 */
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept {
        return mutation != PersistentEdgeMutation::invalid && error.empty();
    }
};

[[nodiscard]] auto local_authority_ready(RC::Unreal::UObject* worldContext, std::string& error)
    -> bool;
[[nodiscard]] auto discover_catalog(RC::Unreal::UObject* worldContext, std::uint64_t generation,
                                    std::span<const PersistentUnionEdge> appliedEdges = {})
    -> ResourceCatalogSnapshot;
[[nodiscard]] auto persistent_modules(const ResourceCatalogSnapshot& catalog)
    -> std::vector<PersistentStorageModule>;
[[nodiscard]] auto apply_persistent_edge(RC::Unreal::UObject* worldContext,
                                         const ResourceCatalogSnapshot& catalog,
                                         const PersistentUnionEdge& edge)
    -> PersistentEdgeMutationResult;
[[nodiscard]] auto remove_persistent_edge(RC::Unreal::UObject* worldContext,
                                          const ResourceCatalogSnapshot& catalog,
                                          const PersistentUnionEdge& edge)
    -> PersistentEdgeMutationResult;
[[nodiscard]] auto read_base_id(RC::Unreal::UObject* baseModel) -> std::optional<GuidKey>;
[[nodiscard]] auto resolve_inside_base_id(RC::Unreal::UObject* worldContext,
                                          const ResourceCatalogSnapshot& catalog,
                                          std::string& error) -> std::optional<GuidKey>;
[[nodiscard]] auto apply_union(RC::Unreal::UObject* worldContext,
                               const ResourceCatalogSnapshot& catalog,
                               const ResourceExposurePlan& exposure, LiveUnion& liveUnion,
                               std::string& error) -> bool;
[[nodiscard]] auto validate_union(const LiveUnion& liveUnion, std::string& error) -> bool;
[[nodiscard]] auto notify_building_inventory_changed(RC::Unreal::UObject* buildModel,
                                                     RC::Unreal::UObject* worldContext,
                                                     const ResourceCatalogSnapshot& catalog,
                                                     const LiveUnion& liveUnion, std::string& error)
    -> bool;
[[nodiscard]] auto restore_union(LiveUnion& liveUnion, std::string& error) -> bool;
}  // namespace base_resource_sharing::detail
