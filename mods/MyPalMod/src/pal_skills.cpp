#include "pal_skills.hpp"

#include "pal_game.hpp"

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/Core/Containers/Array.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include <algorithm>
#include <cstdint>
#include <string>

using namespace RC;
using namespace RC::Unreal;

namespace
{
enum class EPalWazaID : std::uint16_t
{
};

[[nodiscard]] auto to_pal(const skill_editor::SkillTarget target) -> UObject*
{
    auto* pal = reinterpret_cast<UObject*>(target);
    return pal_game::is_valid(pal) ? pal : nullptr;
}

[[nodiscard]] auto narrow_id(const std::wstring& value) -> std::string
{
    std::string result;
    result.reserve(value.size());
    for (const auto character : value)
    {
        result.push_back(character >= 0 && character <= 0x7F ? static_cast<char>(character) : '?');
    }
    return result;
}

[[nodiscard]] auto widen_id(const std::string_view value) -> std::wstring
{
    std::wstring result;
    result.reserve(value.size());
    for (const auto character : value)
    {
        result.push_back(static_cast<wchar_t>(static_cast<unsigned char>(character)));
    }
    return result;
}

template <typename T>
[[nodiscard]] auto find_function(const wchar_t* path) -> T*
{
    return UObjectGlobals::StaticFindObject<T*>(nullptr, nullptr, path);
}
}

namespace pal_skills
{
auto PalSkillGateway::is_valid(const skill_editor::SkillTarget target) const -> bool
{
    return to_pal(target) != nullptr;
}

auto PalSkillGateway::read_state(const skill_editor::SkillTarget target) -> skill_editor::SkillState
{
    skill_editor::SkillState state;
    auto* pal = to_pal(target);
    if (pal == nullptr)
    {
        return state;
    }

    if (auto* function = find_function<UFunction>(
            STR("/Script/Pal.PalIndividualCharacterParameter:GetPassiveSkillList")))
    {
        struct Params
        {
            TArray<FName> ReturnValue;
        } params;
        pal->ProcessEvent(function, &params);

        state.passiveIds.reserve(static_cast<std::size_t>(std::max(params.ReturnValue.Num(), 0)));
        for (int32 index = 0; index < params.ReturnValue.Num(); ++index)
        {
            state.passiveIds.push_back(narrow_id(params.ReturnValue[index].ToString()));
        }
    }

    if (auto* function =
            find_function<UFunction>(STR("/Script/Pal.PalIndividualCharacterParameter:GetEquipWaza")))
    {
        struct Params
        {
            TArray<EPalWazaID> ReturnValue;
        } params;
        pal->ProcessEvent(function, &params);

        const auto count = std::min<int32>(params.ReturnValue.Num(), 3);
        state.activeSkills.reserve(static_cast<std::size_t>(std::max(count, 0)));
        for (int32 index = 0; index < count; ++index)
        {
            const auto value = static_cast<std::uint16_t>(params.ReturnValue[index]);
            state.activeSkills.push_back({.value = value, .id = std::to_string(value)});
        }
        if (params.ReturnValue.Num() > 3)
        {
            Output::send<LogLevel::Warning>(
                STR("MyPalMod: GetEquipWaza returned {} entries; only the first 3 are editable\n"),
                params.ReturnValue.Num());
        }
    }

    return state;
}

auto PalSkillGateway::add_passive(
    const skill_editor::SkillTarget target,
    const std::string_view id) -> bool
{
    auto* pal = to_pal(target);
    auto* function =
        find_function<UFunction>(STR("/Script/Pal.PalIndividualCharacterParameter:AddPassiveSkill"));
    if (pal == nullptr || function == nullptr || id.empty())
    {
        return false;
    }

    struct Params
    {
        FName AddSkill;
        FName OverrideSkill;
    } params;
    const auto wide = widen_id(id);
    params.AddSkill = FName(wide.c_str());
    pal->ProcessEvent(function, &params);
    return true;
}

auto PalSkillGateway::remove_passive(
    const skill_editor::SkillTarget target,
    const std::string_view id) -> bool
{
    auto* pal = to_pal(target);
    auto* function =
        find_function<UFunction>(STR("/Script/Pal.PalIndividualCharacterParameter:RemovePassiveSkill"));
    if (pal == nullptr || function == nullptr || id.empty())
    {
        return false;
    }

    struct Params
    {
        FName SkillId;
    } params;
    const auto wide = widen_id(id);
    params.SkillId = FName(wide.c_str());
    pal->ProcessEvent(function, &params);
    return true;
}

auto PalSkillGateway::rewrite_active(
    const skill_editor::SkillTarget target,
    const std::span<const skill_editor::ActiveSkill> skills) -> bool
{
    auto* pal = to_pal(target);
    auto* clearFunction =
        find_function<UFunction>(STR("/Script/Pal.PalIndividualCharacterParameter:ClearEquipWaza"));
    auto* addFunction =
        find_function<UFunction>(STR("/Script/Pal.PalIndividualCharacterParameter:AddEquipWaza"));
    if (pal == nullptr || clearFunction == nullptr || addFunction == nullptr || skills.size() > 3)
    {
        return false;
    }

    pal->ProcessEvent(clearFunction, nullptr);
    for (const auto& skill : skills)
    {
        if (!pal_game::is_valid(pal))
        {
            return false;
        }
        struct Params
        {
            EPalWazaID WazaId;
        } params{.WazaId = static_cast<EPalWazaID>(skill.value)};
        pal->ProcessEvent(addFunction, &params);
    }
    return true;
}
}
