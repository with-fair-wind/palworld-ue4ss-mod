#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <skills/skill_editor_service.hpp>

namespace skill_editor {
struct PassiveSkillPreset {
    std::string_view id;
    std::string_view displayName;
    std::array<std::string_view, 4> passiveIds;
};

inline constexpr std::array kPassiveSkillPresets{
    PassiveSkillPreset{
        .id = "work-perfect-1",
        .displayName = "工作毕业1",
        .passiveIds = {"WorldTree_CraftSpeed", "CraftSpeed_up3", "Vampire", "CraftSpeed_up2"},
    },
    PassiveSkillPreset{
        .id = "work-perfect-2",
        .displayName = "工作毕业2",
        .passiveIds = {"WorldTree_CraftSpeed", "CraftSpeed_up3", "CraftSpeed_up2",
                       "PAL_CorporateSlave"},
    },
};

[[nodiscard]] constexpr auto passive_skill_presets() noexcept
    -> std::span<const PassiveSkillPreset> {
    return kPassiveSkillPresets;
}

[[nodiscard]] constexpr auto passive_skill_presets_are_valid() noexcept -> bool {
    for (std::size_t presetIndex{}; presetIndex < kPassiveSkillPresets.size(); ++presetIndex) {
        const auto& preset = kPassiveSkillPresets[presetIndex];
        if (preset.id.empty() || preset.displayName.empty()) {
            return false;
        }
        for (std::size_t previousPreset{}; previousPreset < presetIndex; ++previousPreset) {
            if (kPassiveSkillPresets[previousPreset].id == preset.id) {
                return false;
            }
        }
        for (std::size_t passiveIndex{}; passiveIndex < preset.passiveIds.size(); ++passiveIndex) {
            if (preset.passiveIds[passiveIndex].empty()) {
                return false;
            }
            for (std::size_t previousPassive{}; previousPassive < passiveIndex; ++previousPassive) {
                if (preset.passiveIds[previousPassive] == preset.passiveIds[passiveIndex]) {
                    return false;
                }
            }
        }
    }
    return true;
}

static_assert(passive_skill_presets_are_valid());

[[nodiscard]] inline auto make_passive_preset_request(const PassiveSkillPreset& preset,
                                                      const std::uint64_t targetGeneration,
                                                      const std::uint64_t worldGeneration)
    -> SkillEditRequest {
    return {
        .targetGeneration = targetGeneration,
        .worldGeneration = worldGeneration,
        .kind = SkillKind::passive,
        .operation = SkillEditOperation::replaceAllPassives,
        .desiredPassiveIds =
            std::vector<std::string>(preset.passiveIds.begin(), preset.passiveIds.end()),
    };
}
}  // namespace skill_editor
