#pragma once

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <base_resource_sharing/resource_pool.hpp>

namespace base_resource_sharing {
enum class HookAction : std::uint8_t {
    structureChanged,
    buildingModeChanged,
    buildingTouch,
    craftingAcquire,
    craftingTouch,
};

enum class HookRequirement : std::uint8_t { optional, required };

struct HookSpec {
    ResourceOperation operation;
    HookAction action;
    HookRequirement requirement;
    std::string_view path;
};

struct HookResolution {
    HookSpec spec;
    bool resolved{};
};

inline constexpr std::array kPalworld101HookManifest{
    HookSpec{ResourceOperation::repair, HookAction::structureChanged, HookRequirement::optional,
             "/Script/Pal.PalBaseCampModuleItemStorage:OnRep_ContainerInfos"},
    HookSpec{ResourceOperation::repair, HookAction::structureChanged, HookRequirement::optional,
             "/Script/Pal.PalBaseCampModuleItemStorage:OnAvailableConcreteModel_ServerInternal"},
    HookSpec{ResourceOperation::repair, HookAction::structureChanged, HookRequirement::optional,
             "/Script/Pal.PalBaseCampModuleItemStorage:OnNotAvailableConcreteModel_ServerInternal"},
    HookSpec{ResourceOperation::repair, HookAction::structureChanged, HookRequirement::optional,
             "/Script/Pal.PalBaseCampModel:OnRep_ModuleArray"},
    HookSpec{ResourceOperation::building, HookAction::buildingModeChanged,
             HookRequirement::required, "/Script/Pal.PalBuilderComponent:ChangeMode"},
    HookSpec{ResourceOperation::building, HookAction::buildingTouch, HookRequirement::required,
             "/Script/Pal.PalBuilderComponent:IsExistsMaterialForBuildObject"},
    HookSpec{ResourceOperation::crafting, HookAction::craftingAcquire, HookRequirement::required,
             "/Script/Pal.PalUIConvertItemModel:Initialize"},
    HookSpec{ResourceOperation::crafting, HookAction::craftingTouch, HookRequirement::optional,
             "/Script/Pal.PalUIProductSettingModel:CalcMaxProductableNum"},
    HookSpec{ResourceOperation::crafting, HookAction::craftingTouch, HookRequirement::required,
             "/Script/Pal.PalUIConvertItemModel:CanStartProduction"},
    HookSpec{ResourceOperation::crafting, HookAction::craftingTouch, HookRequirement::required,
             "/Script/Pal.PalUIConvertItemModel:StartProduction"},
};

[[nodiscard]] constexpr auto palworld_1_0_1_hook_manifest() noexcept -> std::span<const HookSpec> {
    return kPalworld101HookManifest;
}

[[nodiscard]] inline auto all_hook_resolutions(const bool resolved) -> std::vector<HookResolution> {
    std::vector<HookResolution> result;
    result.reserve(kPalworld101HookManifest.size());
    for (const auto& spec : kPalworld101HookManifest) {
        result.push_back({.spec = spec, .resolved = resolved});
    }
    return result;
}

inline void mark_resolved(const std::span<HookResolution> resolutions,
                          const std::string_view path) {
    for (auto& resolution : resolutions) {
        if (resolution.spec.path == path) {
            resolution.resolved = true;
        }
    }
}

[[nodiscard]] inline auto evaluate_capabilities(const std::span<const HookResolution> resolutions)
    -> std::array<CapabilityState, 3> {
    std::array<CapabilityState, 3> result{};
    std::array<std::size_t, 3> requiredCounts{};
    std::array<std::size_t, 3> resolvedRequiredCounts{};
    for (const auto& resolution : resolutions) {
        if (resolution.spec.requirement != HookRequirement::required) {
            continue;
        }
        auto& capability = result[operation_index(resolution.spec.operation)];
        ++requiredCounts[operation_index(resolution.spec.operation)];
        if (resolution.resolved) {
            ++resolvedRequiredCounts[operation_index(resolution.spec.operation)];
        } else {
            if (!capability.error.empty()) {
                capability.error += "\n";
            }
            capability.error += "缺少必需接口：" + std::string{resolution.spec.path};
        }
    }

    for (const auto operation : {ResourceOperation::crafting, ResourceOperation::building}) {
        const auto index = operation_index(operation);
        const bool ready =
            requiredCounts[index] != 0 && requiredCounts[index] == resolvedRequiredCounts[index];
        result[index].previewReady = ready;
        result[index].consumeReady = ready;
    }

    auto& repair = result[operation_index(ResourceOperation::repair)];
    repair = {.error = "Palworld 1.0.1 尚未验证安全的修理材料检查与扣除入口。"};
    return result;
}
}  // namespace base_resource_sharing
