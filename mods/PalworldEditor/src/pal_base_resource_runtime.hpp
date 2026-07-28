#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

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
    std::size_t sameGuildBaseCount{};
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

[[nodiscard]] auto local_authority_ready(RC::Unreal::UObject* worldContext, std::string& error)
    -> bool;
[[nodiscard]] auto discover_catalog(RC::Unreal::UObject* worldContext, std::uint64_t generation)
    -> ResourceCatalogSnapshot;
[[nodiscard]] auto read_base_id(RC::Unreal::UObject* baseModel) -> std::optional<GuidKey>;
[[nodiscard]] auto resolve_nearest_base_id(RC::Unreal::UObject* worldContext,
                                           const ResourceCatalogSnapshot& catalog,
                                           std::string& error) -> std::optional<GuidKey>;
[[nodiscard]] auto apply_union(RC::Unreal::UObject* worldContext,
                               const ResourceCatalogSnapshot& catalog,
                               const ResourceExposurePlan& exposure, LiveUnion& liveUnion,
                               std::string& error) -> bool;
[[nodiscard]] auto restore_union(LiveUnion& liveUnion, std::string& error) -> bool;
}  // namespace base_resource_sharing::detail
