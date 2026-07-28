#pragma once

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <base_resource_sharing/resource_pool.hpp>

namespace base_resource_sharing {
enum class HookEvent : std::uint8_t {
    none,
    structureChanged,
    acquire,
    touch,
    release,
    updateBuildingMode,
    enterBase,
    exitBase,
};

enum class HookPhase : std::uint8_t { pre, post };
enum class HookRequirement : std::uint8_t { optional, required };

struct HookSpec {
    ResourceOperation operation;
    HookEvent preEvent{HookEvent::none};
    HookEvent postEvent{HookEvent::none};
    HookRequirement requirement;
    std::string_view path;
};

struct HookResolution {
    HookSpec spec;
    bool resolved{};
};

inline constexpr std::array kPalworld101HookManifest{
    HookSpec{ResourceOperation::repair, HookEvent::none, HookEvent::structureChanged,
             HookRequirement::optional,
             "/Script/Pal.PalBaseCampModuleItemStorage:OnRep_ContainerInfos"},
    HookSpec{ResourceOperation::repair, HookEvent::none, HookEvent::structureChanged,
             HookRequirement::optional,
             "/Script/Pal.PalBaseCampModuleItemStorage:OnAvailableConcreteModel_ServerInternal"},
    HookSpec{ResourceOperation::repair, HookEvent::none, HookEvent::structureChanged,
             HookRequirement::optional,
             "/Script/Pal.PalBaseCampModuleItemStorage:OnNotAvailableConcreteModel_ServerInternal"},
    HookSpec{ResourceOperation::repair, HookEvent::none, HookEvent::structureChanged,
             HookRequirement::optional, "/Script/Pal.PalBaseCampModel:OnRep_ModuleArray"},
    HookSpec{ResourceOperation::building, HookEvent::enterBase, HookEvent::none,
             HookRequirement::required, "/Script/Pal.PalBuilderComponent:OnEnterBaseCamp"},
    HookSpec{ResourceOperation::building, HookEvent::exitBase, HookEvent::none,
             HookRequirement::required, "/Script/Pal.PalBuilderComponent:OnExitBaseCamp"},
    HookSpec{ResourceOperation::building, HookEvent::acquire, HookEvent::none,
             HookRequirement::required, "/Script/Pal.PalUIBuildModel:OnOpenMenu"},
    HookSpec{ResourceOperation::building, HookEvent::acquire, HookEvent::none,
             HookRequirement::optional, "/Script/Pal.PalUIInGameMainMenuBuildModel:Setup"},
    HookSpec{ResourceOperation::building, HookEvent::touch, HookEvent::none,
             HookRequirement::optional,
             "/Script/Pal.PalUIBuildModel:GetBuildObjectDataArrayForUIDisplay"},
    HookSpec{ResourceOperation::building, HookEvent::touch, HookEvent::none,
             HookRequirement::required,
             "/Script/Pal.PalBuilderComponent:IsExistsMaterialForBuildObject"},
    HookSpec{ResourceOperation::building, HookEvent::touch, HookEvent::none,
             HookRequirement::required, "/Script/Pal.PalUIBuildModel:StartBuildObject"},
    HookSpec{ResourceOperation::building, HookEvent::touch, HookEvent::none,
             HookRequirement::optional, "/Script/Pal.PalUIBuildingModel:Setup"},
    HookSpec{ResourceOperation::building, HookEvent::touch, HookEvent::none,
             HookRequirement::optional, "/Script/Pal.PalUIBuildingModel:BuildObject"},
    HookSpec{ResourceOperation::building, HookEvent::none, HookEvent::updateBuildingMode,
             HookRequirement::required, "/Script/Pal.PalBuilderComponent:ChangeMode"},
    HookSpec{ResourceOperation::crafting, HookEvent::acquire, HookEvent::none,
             HookRequirement::required, "/Script/Pal.PalUIConvertItemModel:Initialize"},
    HookSpec{ResourceOperation::crafting, HookEvent::touch, HookEvent::none,
             HookRequirement::optional, "/Script/Pal.PalUIProductSettingModel:SelectRecipe"},
    HookSpec{ResourceOperation::crafting, HookEvent::touch, HookEvent::none,
             HookRequirement::optional, "/Script/Pal.PalUIProductSettingModel:SetFocusedRecipe"},
    HookSpec{ResourceOperation::crafting, HookEvent::touch, HookEvent::none,
             HookRequirement::optional,
             "/Script/Pal.PalUIProductSettingModel:CalcMaxProductableNum"},
    HookSpec{ResourceOperation::crafting, HookEvent::touch, HookEvent::none,
             HookRequirement::required, "/Script/Pal.PalUIConvertItemModel:CanStartProduction"},
    HookSpec{ResourceOperation::crafting, HookEvent::touch, HookEvent::release,
             HookRequirement::required, "/Script/Pal.PalUIConvertItemModel:StartProduction"},
};

[[nodiscard]] constexpr auto palworld_1_0_1_hook_manifest() noexcept -> std::span<const HookSpec> {
    return kPalworld101HookManifest;
}

[[nodiscard]] constexpr auto event_for_phase(const HookSpec& spec, const HookPhase phase) noexcept
    -> HookEvent {
    return phase == HookPhase::pre ? spec.preEvent : spec.postEvent;
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
