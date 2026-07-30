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
    ensurePersistentUnion,
    validatePersistentUnion,
};

enum class HookPhase : std::uint8_t { pre, post };
enum class HookRequirement : std::uint8_t { optional, required };
enum class ResourceHookBackend : std::uint8_t { nativeFunction, scriptFunction, unsupported };

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
    HookSpec{ResourceOperation::building, HookEvent::none, HookEvent::structureChanged,
             HookRequirement::required,
             "/Script/Pal.PalBaseCampModuleItemStorage:"
             "OnAvailableConcreteModel_ServerInternal"},
    HookSpec{ResourceOperation::building, HookEvent::none, HookEvent::structureChanged,
             HookRequirement::required,
             "/Script/Pal.PalBaseCampModuleItemStorage:"
             "OnNotAvailableConcreteModel_ServerInternal"},
    HookSpec{ResourceOperation::building, HookEvent::none, HookEvent::structureChanged,
             HookRequirement::required, "/Script/Pal.PalBaseCampModel:OnRep_ModuleArray"},
    HookSpec{ResourceOperation::building, HookEvent::ensurePersistentUnion, HookEvent::none,
             HookRequirement::optional, "/Script/Pal.PalUIBuildModel:OnOpenMenu"},
    HookSpec{ResourceOperation::building, HookEvent::validatePersistentUnion, HookEvent::none,
             HookRequirement::required,
             "/Script/Pal.PalNetworkPlayerComponent:RequestBuild_ToServer"},
    HookSpec{ResourceOperation::crafting, HookEvent::ensurePersistentUnion, HookEvent::none,
             HookRequirement::optional, "/Script/Pal.PalUIConvertItemModel:Initialize"},
    HookSpec{ResourceOperation::crafting, HookEvent::validatePersistentUnion, HookEvent::none,
             HookRequirement::required, "/Script/Pal.PalUIConvertItemModel:StartProduction"},
};

[[nodiscard]] constexpr auto palworld_1_0_1_hook_manifest() noexcept -> std::span<const HookSpec> {
    return kPalworld101HookManifest;
}

/**
 * @brief 选择不会经过 UObjectGlobals 全名散列表分发的 Hook 后端。
 * @param hasFunctionPointer UFunction 是否具有底层调用入口。
 * @param usesProcessInternal 底层入口是否为 Blueprint VM 的 ProcessInternal。
 * @param nativeFlag UFunction 是否带 FUNC_Native。
 */
[[nodiscard]] constexpr auto select_resource_hook_backend(const bool hasFunctionPointer,
                                                          const bool usesProcessInternal,
                                                          const bool nativeFlag) noexcept
    -> ResourceHookBackend {
    if (!hasFunctionPointer) {
        return ResourceHookBackend::unsupported;
    }
    if (!usesProcessInternal && nativeFlag) {
        return ResourceHookBackend::nativeFunction;
    }
    if (usesProcessInternal && !nativeFlag) {
        return ResourceHookBackend::scriptFunction;
    }
    return ResourceHookBackend::unsupported;
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
    std::size_t globalRequiredCount{};
    std::size_t resolvedGlobalRequiredCount{};
    std::string globalError;
    for (const auto& resolution : resolutions) {
        if (resolution.spec.requirement != HookRequirement::required) {
            continue;
        }
        const bool global = resolution.spec.preEvent == HookEvent::structureChanged ||
                            resolution.spec.postEvent == HookEvent::structureChanged;
        if (global) {
            ++globalRequiredCount;
            if (resolution.resolved) {
                ++resolvedGlobalRequiredCount;
            } else {
                if (!globalError.empty()) {
                    globalError += "\n";
                }
                globalError += "缺少必需接口：" + std::string{resolution.spec.path};
            }
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
            globalRequiredCount != 0 && globalRequiredCount == resolvedGlobalRequiredCount &&
            requiredCounts[index] != 0 && requiredCounts[index] == resolvedRequiredCounts[index];
        result[index].previewReady = ready;
        result[index].consumeReady = ready;
        if (!globalError.empty()) {
            if (!result[index].error.empty()) {
                result[index].error += "\n";
            }
            result[index].error += globalError;
        }
    }

    auto& repair = result[operation_index(ResourceOperation::repair)];
    repair = {.error = "Palworld 1.0.1 尚未验证安全的修理材料检查与扣除入口。"};
    return result;
}
}  // namespace base_resource_sharing
