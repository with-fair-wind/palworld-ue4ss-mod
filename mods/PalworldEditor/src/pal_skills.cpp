#include <skills/pal_skills.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_set>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/Core/Containers/Array.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/FText.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include <game/pal_game.hpp>
#include <support/text_encoding.hpp>

using namespace RC;
using namespace RC::Unreal;

namespace {
enum class EPalWazaID : std::uint16_t {};

[[nodiscard]] auto to_pal(const skill_editor::SkillTarget target) -> UObject* {
    auto* pal = reinterpret_cast<UObject*>(target);
    return pal_game::is_valid(pal) ? pal : nullptr;
}

template <typename T>
[[nodiscard]] auto find_function(const wchar_t* path) -> T* {
    return UObjectGlobals::StaticFindObject<T*>(nullptr, nullptr, path);
}

[[nodiscard]] auto ui_utility() -> UObject* {
    if (auto* utility = UObjectGlobals::StaticFindObject<UObject*>(
            nullptr, nullptr, STR("/Script/Pal.Default__PalUIUtility"))) {
        return utility;
    }
    return UObjectGlobals::FindFirstOf(STR("PalUIUtility"));
}

[[nodiscard]] auto passive_localized_name(UObject* utility, UObject* worldContext, const FName& id)
    -> std::string {
    auto* function = find_function<UFunction>(STR("/Script/Pal.PalUIUtility:GetPassiveSkillName"));
    if (utility == nullptr || function == nullptr) {
        return {};
    }

    struct Params {
        UObject* WorldContextObject;
        FName PassiveSkillId;
        FText OutName;
    } params{.WorldContextObject = worldContext, .PassiveSkillId = id};
    utility->ProcessEvent(function, &params);
    return text_encoding::to_utf8(params.OutName.ToString());
}

[[nodiscard]] auto active_localized_name(UObject* utility, UObject* worldContext,
                                         const EPalWazaID id) -> std::string {
    auto* function = find_function<UFunction>(STR("/Script/Pal.PalUIUtility:GetWazaName"));
    if (utility == nullptr || function == nullptr) {
        return {};
    }

    struct Params {
        UObject* WorldContextObject;
        EPalWazaID WazaId;
        FText OutName;
    } params{.WorldContextObject = worldContext, .WazaId = id};
    utility->ProcessEvent(function, &params);
    return text_encoding::to_utf8(params.OutName.ToString());
}

[[nodiscard]] auto strip_enum_prefix(std::string name) -> std::string {
    if (const auto separator = name.rfind("::"); separator != std::string::npos) {
        name.erase(0, separator + 2);
    }
    return name;
}

[[nodiscard]] auto is_active_sentinel(const std::string_view id) -> bool {
    const auto lowered = skill_editor::ascii_lower(id);
    return lowered.empty() || lowered == "none" || lowered == "max" || lowered.ends_with("_max");
}
}  // namespace

namespace pal_skills {
auto PalSkillGateway::is_valid(const skill_editor::SkillTarget target) const -> bool {
    return to_pal(target) != nullptr;
}

auto PalSkillGateway::read_state(const skill_editor::SkillTarget target)
    -> skill_editor::SkillState {
    skill_editor::SkillState state;
    auto* pal = to_pal(target);
    if (pal == nullptr) {
        return state;
    }

    if (auto* function = find_function<UFunction>(
            STR("/Script/Pal.PalIndividualCharacterParameter:GetPassiveSkillList"))) {
        struct Params {
            TArray<FName> ReturnValue;
        } params;
        pal->ProcessEvent(function, &params);

        state.passiveIds.reserve(static_cast<std::size_t>(std::max(params.ReturnValue.Num(), 0)));
        for (int32 index = 0; index < params.ReturnValue.Num(); ++index) {
            state.passiveIds.push_back(
                text_encoding::to_utf8(params.ReturnValue[index].ToString()));
        }
    }

    if (auto* function = find_function<UFunction>(
            STR("/Script/Pal.PalIndividualCharacterParameter:GetEquipWaza"))) {
        struct Params {
            TArray<EPalWazaID> ReturnValue;
        } params;
        pal->ProcessEvent(function, &params);

        const auto count = std::min<int32>(params.ReturnValue.Num(), 3);
        state.activeSkills.reserve(static_cast<std::size_t>(std::max(count, 0)));
        for (int32 index = 0; index < count; ++index) {
            const auto value = static_cast<std::uint16_t>(params.ReturnValue[index]);
            const auto found = activeIds_.find(value);
            state.activeSkills.push_back(
                {.value = value,
                 .id = found == activeIds_.end() ? std::to_string(value) : found->second});
        }
        if (params.ReturnValue.Num() > 3) {
            Output::send<LogLevel::Warning>(
                STR("MyPalMod: GetEquipWaza returned {} entries; only the first 3 are editable\n"),
                params.ReturnValue.Num());
        }
    }

    return state;
}

auto PalSkillGateway::add_passive(const skill_editor::SkillTarget target, const std::string_view id)
    -> bool {
    auto* pal = to_pal(target);
    auto* function = find_function<UFunction>(
        STR("/Script/Pal.PalIndividualCharacterParameter:AddPassiveSkill"));
    if (pal == nullptr || function == nullptr || id.empty()) {
        return false;
    }

    struct Params {
        FName AddSkill;
        FName OverrideSkill;
    } params;
    const auto wide = text_encoding::widen_ascii(id);
    params.AddSkill = FName(wide.c_str());
    pal->ProcessEvent(function, &params);
    return true;
}

auto PalSkillGateway::remove_passive(const skill_editor::SkillTarget target,
                                     const std::string_view id) -> bool {
    auto* pal = to_pal(target);
    auto* function = find_function<UFunction>(
        STR("/Script/Pal.PalIndividualCharacterParameter:RemovePassiveSkill"));
    if (pal == nullptr || function == nullptr || id.empty()) {
        return false;
    }

    struct Params {
        FName SkillId;
    } params;
    const auto wide = text_encoding::widen_ascii(id);
    params.SkillId = FName(wide.c_str());
    pal->ProcessEvent(function, &params);
    return true;
}

auto PalSkillGateway::rewrite_active(const skill_editor::SkillTarget target,
                                     const std::span<const skill_editor::ActiveSkill> skills)
    -> bool {
    auto* pal = to_pal(target);
    auto* clearFunction =
        find_function<UFunction>(STR("/Script/Pal.PalIndividualCharacterParameter:ClearEquipWaza"));
    auto* addFunction =
        find_function<UFunction>(STR("/Script/Pal.PalIndividualCharacterParameter:AddEquipWaza"));
    if (pal == nullptr || clearFunction == nullptr || addFunction == nullptr || skills.size() > 3) {
        return false;
    }

    pal->ProcessEvent(clearFunction, nullptr);
    for (const auto& skill : skills) {
        if (!pal_game::is_valid(pal)) {
            return false;
        }
        struct Params {
            EPalWazaID WazaId;
        } params{.WazaId = static_cast<EPalWazaID>(skill.value)};
        pal->ProcessEvent(addFunction, &params);
    }
    return true;
}

auto PalSkillGateway::load_catalog(const skill_editor::SkillTarget contextTarget)
    -> skill_editor::SkillCatalogSnapshot {
    skill_editor::SkillCatalogSnapshot catalog;
    auto* worldContext = to_pal(contextTarget);
    auto* utility = ui_utility();

    auto* manager = UObjectGlobals::FindFirstOf(STR("PalPassiveSkillManager"));
    auto* passiveFunction = find_function<UFunction>(
        STR("/Script/Pal.PalPassiveSkillManager:GetPalAssignablePassiveIDs"));
    if (manager != nullptr && passiveFunction != nullptr) {
        struct Params {
            TArray<FName> List;
        } params;
        manager->ProcessEvent(passiveFunction, &params);
        catalog.passiveSkills.reserve(static_cast<std::size_t>(std::max(params.List.Num(), 0)));
        for (int32 index = 0; index < params.List.Num(); ++index) {
            const auto& id = params.List[index];
            catalog.passiveSkills.push_back(
                {.id = text_encoding::to_utf8(id.ToString()),
                 .localizedName = passive_localized_name(utility, worldContext, id)});
        }
        catalog.passiveSkills = skill_editor::deduplicate_skills(std::move(catalog.passiveSkills));
    }

    if (auto* enumeration = UObjectGlobals::StaticFindObject<UEnum*>(
            nullptr, nullptr, STR("/Script/Pal.EPalWazaID"))) {
        auto names = enumeration->GetEnumNames();
        std::unordered_set<std::uint16_t> seenValues;
        catalog.activeSkills.reserve(static_cast<std::size_t>(std::max(names.Num(), 0)));
        for (int32 index = 0; index < names.Num(); ++index) {
            const auto& pair = names[index];
            if (pair.Value < 0 || pair.Value > std::numeric_limits<std::uint16_t>::max()) {
                continue;
            }

            auto id = strip_enum_prefix(text_encoding::to_utf8(pair.Key.ToString()));
            const auto value = static_cast<std::uint16_t>(pair.Value);
            if (is_active_sentinel(id) || !seenValues.insert(value).second) {
                continue;
            }
            catalog.activeSkills.push_back(
                {.id = std::move(id),
                 .localizedName =
                     active_localized_name(utility, worldContext, static_cast<EPalWazaID>(value)),
                 .activeValue = value});
        }
    }

    if (catalog.passiveSkills.empty()) {
        catalog.error = "Unable to load Pal-assignable passive skills";
        return catalog;
    }
    if (catalog.activeSkills.empty()) {
        catalog.error = "Unable to load EPalWazaID active skills";
        return catalog;
    }

    const auto byLabel = [](const skill_editor::SkillOption& left,
                            const skill_editor::SkillOption& right) {
        return skill_editor::ascii_lower(skill_editor::skill_label(left)) <
               skill_editor::ascii_lower(skill_editor::skill_label(right));
    };
    std::ranges::sort(catalog.passiveSkills, byLabel);
    std::ranges::sort(catalog.activeSkills, byLabel);

    activeIds_.clear();
    for (const auto& option : catalog.activeSkills) {
        if (option.activeValue.has_value()) {
            activeIds_.insert_or_assign(*option.activeValue, option.id);
        }
    }
    catalog.ready = true;
    return catalog;
}
}  // namespace pal_skills
