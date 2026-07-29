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
    validate,
    refreshBuilding,
    closeCrafting,
    updateBuildingMode,
    enterBase,
    exitBase,
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
    HookSpec{ResourceOperation::building, HookEvent::acquire, HookEvent::none,
             HookRequirement::required,
             "/Script/Pal.PalUIBuildModel:GetBuildObjectDataArrayForUIDisplay"},
    HookSpec{ResourceOperation::building, HookEvent::acquire, HookEvent::none,
             HookRequirement::required,
             "/Script/Pal.PalBuilderComponent:IsExistsMaterialForBuildObject"},
    HookSpec{ResourceOperation::building, HookEvent::acquire, HookEvent::refreshBuilding,
             HookRequirement::required, "/Script/Pal.PalUIInGameMainMenuBuildModel:Setup"},
    HookSpec{ResourceOperation::building, HookEvent::validate, HookEvent::none,
             HookRequirement::required,
             "/Script/Pal.PalNetworkPlayerComponent:RequestBuild_ToServer"},
    HookSpec{ResourceOperation::building, HookEvent::none, HookEvent::updateBuildingMode,
             HookRequirement::required, "/Script/Pal.PalBuilderComponent:ChangeMode"},
    HookSpec{ResourceOperation::crafting, HookEvent::acquire, HookEvent::none,
             HookRequirement::required, "/Script/Pal.PalUIConvertItemModel:Initialize"},
    HookSpec{ResourceOperation::crafting, HookEvent::validate, HookEvent::none,
             HookRequirement::required, "/Script/Pal.PalUIConvertItemModel:StartProduction"},
    HookSpec{ResourceOperation::crafting, HookEvent::closeCrafting, HookEvent::none,
             HookRequirement::required, "/Script/Pal.PalUserWidget:OnClosed"},
};

/** @brief 精确识别制作界面的 HUD dispatch parameter 对象全名。 */
[[nodiscard]] constexpr auto is_convert_item_dispatch_parameter(
    const std::wstring_view fullName) noexcept -> bool {
    constexpr std::wstring_view prefix{L"PalHUDDispatchParameter_ConvertItem "};
    return fullName.starts_with(prefix);
}

[[nodiscard]] constexpr auto palworld_1_0_1_hook_manifest() noexcept -> std::span<const HookSpec> {
    return kPalworld101HookManifest;
}

/** @return 所有必要前台会话 Hook 是否已注册，完成后不再进行逐帧重试检查。 */
[[nodiscard]] constexpr auto hook_registration_complete(const std::size_t registeredCount) noexcept
    -> bool {
    return registeredCount == kPalworld101HookManifest.size();
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
