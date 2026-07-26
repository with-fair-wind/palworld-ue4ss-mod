#pragma once

#include <cstdint>
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
    std::vector<UnionLedgerEntry> entries;
    bool active{};
};

[[nodiscard]] auto local_authority_ready(RC::Unreal::UObject* worldContext, std::string& error)
    -> bool;
[[nodiscard]] auto discover_catalog(RC::Unreal::UObject* worldContext, std::uint64_t generation)
    -> ResourceCatalogSnapshot;
[[nodiscard]] auto apply_union(RC::Unreal::UObject* worldContext,
                               const ResourceCatalogSnapshot& catalog, LiveUnion& liveUnion,
                               std::string& error) -> bool;
[[nodiscard]] auto restore_union(LiveUnion& liveUnion, std::string& error) -> bool;
}  // namespace base_resource_sharing::detail
