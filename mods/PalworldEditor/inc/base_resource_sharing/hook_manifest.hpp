#pragma once

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <base_resource_sharing/resource_pool.hpp>

namespace base_resource_sharing {
enum class HookRole : std::uint8_t { preview, consume };

struct HookSpec {
    ResourceOperation operation;
    HookRole role;
    std::string_view path;
};

struct HookResolution {
    HookSpec spec;
    bool resolved{};
};

inline constexpr std::array kPalworld101HookManifest{
    HookSpec{ResourceOperation::crafting, HookRole::preview,
             "/Script/Pal.PalUIProductSettingModel:CalcMaxProductableNum"},
    HookSpec{ResourceOperation::crafting, HookRole::consume,
             "/Script/Pal.PalUIConvertItemModel:StartProduction"},
    HookSpec{ResourceOperation::building, HookRole::preview,
             "/Script/Pal.PalBuilderComponent:IsExistsMaterialForBuildObject"},
    HookSpec{ResourceOperation::building, HookRole::consume,
             "/Script/Pal.PalNetworkPlayerComponent:RequestBuild_ToServer"},
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
    for (const auto& resolution : resolutions) {
        auto& capability = result[operation_index(resolution.spec.operation)];
        if (resolution.resolved) {
            if (resolution.spec.role == HookRole::preview) {
                capability.previewReady = true;
            } else if (resolution.spec.role == HookRole::consume) {
                capability.consumeReady = true;
            }
        } else {
            if (!capability.error.empty()) {
                capability.error += "\n";
            }
            capability.error += "缺少必需接口：" + std::string{resolution.spec.path};
        }
    }

    auto& repair = result[operation_index(ResourceOperation::repair)];
    repair = {.error = "Palworld 1.0.1 尚未验证安全的修理材料检查与扣除入口。"};
    return result;
}
}  // namespace base_resource_sharing
