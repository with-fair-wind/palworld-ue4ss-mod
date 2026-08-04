/**
 * @file pal_skills.cpp
 * @brief 实现 Palworld 技能编辑网关及运行时技能目录反射。
 * @details 本文件把无 Unreal 依赖的 `skill_editor` 领域服务映射到
 *          `PalIndividualCharacterParameter`、`PalPassiveSkillManager` 和
 *          `PalUIUtility`。所有接口均在游戏线程执行，所有 Unreal 裸指针均为非拥有观察指针。
 */
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/Core/Containers/Array.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/FText.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <common/text_encoding.hpp>
#include <game/pal_game.hpp>
#include <skills/pal_skills.hpp>

using namespace RC;
using namespace RC::Unreal;

/** @brief 保存只供本翻译单元使用的 Palworld 反射辅助类型和函数。 */
namespace {
/**
 * @brief 与 Palworld 的 `EPalWazaID` 底层布局一致的强类型数值。
 * @details 枚举成员来自生成的纯 C++ 定义表，本地类型只用于匹配反射函数的 16 位参数布局。
 */
enum class EPalWazaID : std::uint16_t {};

/**
 * @brief 把整数目标句柄还原为当前有效的帕鲁 UObject。
 * @param[in] target 由非拥有 UObject 指针编码的技能目标。
 * @return 指向帕鲁对象的非拥有观察指针。
 * @retval nullptr 目标为空或未通过 pal_game::is_valid()。
 */
[[nodiscard]] auto to_pal(const skill_editor::SkillTarget target) -> UObject* {
    auto* pal = reinterpret_cast<UObject*>(target);
    return pal_game::is_valid(pal) ? pal : nullptr;
}

/**
 * @brief 按完整反射路径查找指定 Unreal 字段类型。
 * @tparam T 期望的 Unreal 对象类型，例如 `UFunction`。
 * @param[in] path 以空字符结尾的完整宽字符反射路径。
 * @return 指向已加载对象的非拥有观察指针；未找到时返回 `nullptr`。
 */
template <typename T>
[[nodiscard]] auto find_function(const wchar_t* path) -> T* {
    return UObjectGlobals::StaticFindObject<T*>(nullptr, nullptr, path);
}

/**
 * @brief 查找提供技能本地化名称的 `PalUIUtility` 默认对象。
 * @return 指向 UI 工具对象的非拥有观察指针。
 * @retval nullptr 默认对象和当前已加载对象中均未找到该工具。
 */
[[nodiscard]] auto ui_utility() -> UObject* {
    if (auto* utility = UObjectGlobals::StaticFindObject<UObject*>(
            nullptr, nullptr, STR("/Script/Pal.Default__PalUIUtility"))) {
        return utility;
    }
    return UObjectGlobals::FindFirstOf(STR("PalUIUtility"));
}

/**
 * @brief 查询被动技能在当前游戏语言下的名称。
 * @param[in] utility 非拥有 `PalUIUtility` 对象指针。
 * @param[in] function 非拥有 `GetPassiveSkillName` 函数指针。
 * @param[in] worldContext 非拥有世界上下文对象。
 * @param[in] id 被动技能 Raw ID。
 * @return UTF-8 本地化名称；工具或反射函数不可用、文本转换失败时返回空字符串。
 */
[[nodiscard]] auto passive_localized_name(UObject* utility, UFunction* function,
                                          UObject* worldContext, const FName& id) -> std::string {
    if (utility == nullptr || function == nullptr || worldContext == nullptr) {
        return {};
    }

    /** @brief `PalUIUtility:GetPassiveSkillName` 的反射参数布局。 */
    struct Params {
        UObject* WorldContextObject; /**< 非拥有世界上下文对象。 */
        FName PassiveSkillId;        /**< 要查询的被动技能 Raw ID。 */
        FText OutName;               /**< 游戏函数写回的本地化名称。 */
    } params{.WorldContextObject = worldContext, .PassiveSkillId = id};
    utility->ProcessEvent(function, &params);
    return text_encoding::to_utf8(params.OutName.ToString());
}

/**
 * @brief 查询主动技能在当前游戏语言下的名称。
 * @param[in] utility 非拥有 `PalUIUtility` 对象指针。
 * @param[in] function 非拥有 `GetWazaName` 函数指针。
 * @param[in] worldContext 非拥有世界上下文对象。
 * @param[in] id 主动技能 `EPalWazaID` 数值。
 * @return UTF-8 本地化名称；工具或反射函数不可用、文本转换失败时返回空字符串。
 */
[[nodiscard]] auto active_localized_name(UObject* utility, UFunction* function,
                                         UObject* worldContext, const EPalWazaID id)
    -> std::string {
    if (utility == nullptr || function == nullptr || worldContext == nullptr) {
        return {};
    }

    /** @brief `PalUIUtility:GetWazaName` 的反射参数布局。 */
    struct Params {
        UObject* WorldContextObject; /**< 非拥有世界上下文对象。 */
        EPalWazaID WazaId;           /**< 要查询的主动技能枚举值。 */
        FText OutName;               /**< 游戏函数写回的本地化名称。 */
    } params{.WorldContextObject = worldContext, .WazaId = id};
    utility->ProcessEvent(function, &params);
    return text_encoding::to_utf8(params.OutName.ToString());
}

}  // namespace

/** @brief 实现 Palworld 特定技能网关的成员函数。 */
namespace pal_skills {
/** @details 通过 to_pal() 和 pal_game::is_valid() 执行轻量 UObject 校验。 */
auto PalSkillGateway::is_valid(const skill_editor::SkillTarget target) const -> bool {
    return to_pal(target) != nullptr;
}

/**
 * @details 被动技能通过 `GetPassiveSkillList` 读取；主动技能通过 `GetEquipWaza` 读取，
 *          并限制为可编辑的前三个槽位。完整调用契约见头文件中的成员声明。
 */
auto PalSkillGateway::read_state(const skill_editor::SkillTarget target)
    -> skill_editor::SkillState {
    skill_editor::SkillState state;
    auto* pal = to_pal(target);
    if (pal == nullptr) {
        return state;
    }

    if (auto* function = find_function<UFunction>(
            STR("/Script/Pal.PalIndividualCharacterParameter:GetPassiveSkillList"))) {
        /** @brief `GetPassiveSkillList` 的反射返回布局。 */
        struct Params {
            TArray<FName> ReturnValue; /**< 游戏返回的被动技能 Raw ID 数组。 */
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
        /** @brief `GetEquipWaza` 的反射返回布局。 */
        struct Params {
            TArray<EPalWazaID> ReturnValue; /**< 游戏返回的主动技能槽位枚举数组。 */
        } params;
        pal->ProcessEvent(function, &params);

        const auto count = std::min<int32>(params.ReturnValue.Num(), 3);
        state.activeSkills.reserve(static_cast<std::size_t>(std::max(count, 0)));
        for (int32 index = 0; index < count; ++index) {
            const auto value = static_cast<std::uint16_t>(params.ReturnValue[index]);
            state.activeSkills.push_back(
                {.value = value, .id = skill_editor::active_skill_id_or_numeric(value)});
        }
        if (params.ReturnValue.Num() > 3) {
            Output::send<LogLevel::Warning>(
                STR("PalworldEditor: GetEquipWaza returned {} entries; only the first 3 are "
                    "editable\n"),
                params.ReturnValue.Num());
        }
    }

    return state;
}

/**
 * @details 使用与 UHT 声明一致的 `AddSkill`/`OverrideSkill` 参数布局发起反射调用；
 *          完整调用契约见头文件中的成员声明。
 */
auto PalSkillGateway::add_passive(const skill_editor::SkillTarget target, const std::string_view id)
    -> bool {
    auto* pal = to_pal(target);
    auto* function = find_function<UFunction>(
        STR("/Script/Pal.PalIndividualCharacterParameter:AddPassiveSkill"));
    if (pal == nullptr || function == nullptr || id.empty()) {
        return false;
    }

    /** @brief `AddPassiveSkill` 的反射参数布局。 */
    struct Params {
        FName AddSkill;      /**< 要添加的被动技能 Raw ID。 */
        FName OverrideSkill; /**< 游戏可能写回的覆盖技能 ID；调用方不依赖该值。 */
    } params;
    const auto wide = text_encoding::widen_ascii(id);
    params.AddSkill = FName(wide.c_str());
    pal->ProcessEvent(function, &params);
    return true;
}

/**
 * @details 使用与 UHT 声明一致的 `SkillId` 参数布局发起反射调用；
 *          完整调用契约见头文件中的成员声明。
 */
auto PalSkillGateway::remove_passive(const skill_editor::SkillTarget target,
                                     const std::string_view id) -> bool {
    auto* pal = to_pal(target);
    auto* function = find_function<UFunction>(
        STR("/Script/Pal.PalIndividualCharacterParameter:RemovePassiveSkill"));
    if (pal == nullptr || function == nullptr || id.empty()) {
        return false;
    }

    /** @brief `RemovePassiveSkill` 的反射参数布局。 */
    struct Params {
        FName SkillId; /**< 要移除的被动技能 Raw ID。 */
    } params;
    const auto wide = text_encoding::widen_ascii(id);
    params.SkillId = FName(wide.c_str());
    pal->ProcessEvent(function, &params);
    return true;
}

/**
 * @details 先调用 `ClearEquipWaza`，再按输入顺序逐项调用 `AddEquipWaza`；
 *          完整调用契约见头文件中的成员声明。
 */
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
        /** @brief `AddEquipWaza` 的反射参数布局。 */
        struct Params {
            EPalWazaID WazaId; /**< 要追加到下一个槽位的主动技能枚举值。 */
        } params{.WazaId = static_cast<EPalWazaID>(skill.value)};
        pal->ProcessEvent(addFunction, &params);
    }
    return true;
}

/**
 * @details 动态解析 `GetSkillData` 的参数与返回结构，只读取 `Rank` 和
 *          `AddWorldTreePal`。完整调用契约见头文件中的成员声明。
 */
auto PalSkillGateway::load_passive_skill_metadata_batch(
    const std::span<const std::string> ids, const std::size_t maxItems,
    const std::chrono::microseconds budget) const -> skill_editor::PassiveSkillMetadataBatchResult {
    using clock = std::chrono::steady_clock;
    const auto startedAt = clock::now();
    skill_editor::PassiveSkillMetadataBatchResult result;
    const auto finish = [&result, startedAt] {
        result.elapsed =
            std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - startedAt);
    };

    if (ids.empty() || maxItems == 0) {
        finish();
        return result;
    }

    auto* const manager = UObjectGlobals::FindFirstOf(STR("PalPassiveSkillManager"));
    auto* const function =
        find_function<UFunction>(STR("/Script/Pal.PalPassiveSkillManager:GetSkillData"));
    auto* const skillNameProperty =
        function == nullptr
            ? nullptr
            : CastField<FNameProperty>(function->FindProperty(FName(STR("SkillName"), FNAME_Find)));
    auto* const outSkillDataProperty =
        function == nullptr ? nullptr
                            : CastField<FStructProperty>(
                                  function->FindProperty(FName(STR("outSkillData"), FNAME_Find)));
    auto* const returnProperty =
        function == nullptr ? nullptr : CastField<FBoolProperty>(function->GetReturnProperty());
    auto* const rowStruct =
        outSkillDataProperty == nullptr ? nullptr : outSkillDataProperty->GetStruct().Get();
    auto* const rankProperty =
        rowStruct == nullptr
            ? nullptr
            : CastField<FIntProperty>(rowStruct->FindProperty(FName(STR("Rank"), FNAME_Find)));
    auto* const worldTreeProperty = rowStruct == nullptr
                                        ? nullptr
                                        : CastField<FBoolProperty>(rowStruct->FindProperty(
                                              FName(STR("AddWorldTreePal"), FNAME_Find)));

    if (manager == nullptr) {
        result.error = "PalPassiveSkillManager is unavailable";
    } else if (function == nullptr) {
        result.error = "GetSkillData is unavailable";
    } else if (skillNameProperty == nullptr) {
        result.error = "GetSkillData.SkillName is unavailable";
    } else if (outSkillDataProperty == nullptr || rowStruct == nullptr) {
        result.error = "GetSkillData.outSkillData is unavailable";
    } else if (returnProperty == nullptr) {
        result.error = "GetSkillData return value is unavailable";
    } else if (rankProperty == nullptr) {
        result.error = "FPalPassiveSkillDatabaseRow.Rank is unavailable";
    } else if (worldTreeProperty == nullptr) {
        result.error = "FPalPassiveSkillDatabaseRow.AddWorldTreePal is unavailable";
    }
    if (!result.error.empty()) {
        finish();
        return result;
    }

    const auto count = std::min(ids.size(), maxItems);
    result.entries.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        {
            std::vector<std::byte> params(static_cast<std::size_t>(function->GetParmsSize()));
            function->InitializeStruct(params.data());
            struct ParamsGuard {
                UFunction* function{}; /**< 非拥有的当前调用函数。 */
                void* params{};        /**< 当前调用动态参数缓冲区。 */

                /** @brief 销毁参数缓冲区内由 Unreal 初始化的字段。 */
                ~ParamsGuard() {
                    function->DestroyStruct(params);
                }
            } guard{.function = function, .params = params.data()};

            const auto wide = text_encoding::widen_ascii(ids[index]);
            const FName skillName(wide.c_str());
            skillNameProperty->CopyCompleteValue(
                skillNameProperty->ContainerPtrToValuePtr<void>(params.data()), &skillName);
            manager->ProcessEvent(function, params.data());

            if (!returnProperty->GetPropertyValueInContainer(params.data())) {
                result.entries.push_back({.id = ids[index], .metadata = std::nullopt});
            } else {
                void* const row = outSkillDataProperty->ContainerPtrToValuePtr<void>(params.data());
                const auto rank = rankProperty->GetPropertyValueInContainer(row);
                const bool addWorldTreePal = worldTreeProperty->GetPropertyValueInContainer(row);
                result.entries.push_back(
                    {.id = ids[index],
                     .metadata = skill_editor::PassiveSkillMetadata{
                         .rank = rank,
                         .addWorldTreePal = addWorldTreePal,
                         .category = skill_editor::classify_passive_skill(rank, addWorldTreePal)}});
            }
        }

        if (clock::now() - startedAt >= budget) {
            break;
        }
    }

    finish();
    return result;
}

/**
 * @details 被动技能来自 `PalPassiveSkillManager:GetPalAssignablePassiveIDs`；主动技能来自
 *          生成的 Palworld 1.0 定义表。`PalPlayerInventoryData` 只用于查询当前游戏语言名称，
 *          本地化不可用时目录仍回退到 Raw ID。完整调用契约见头文件中的成员声明。
 */
auto PalSkillGateway::load_catalog() -> skill_editor::SkillCatalogSnapshot {
    skill_editor::SkillCatalogSnapshot catalog;
    auto* const worldContext = UObjectGlobals::FindFirstOf(pal_game::kInventoryClassName);
    auto* const utility = ui_utility();
    auto* const passiveNameFunction =
        find_function<UFunction>(STR("/Script/Pal.PalUIUtility:GetPassiveSkillName"));
    auto* const activeNameFunction =
        find_function<UFunction>(STR("/Script/Pal.PalUIUtility:GetWazaName"));

    auto* const manager = UObjectGlobals::FindFirstOf(STR("PalPassiveSkillManager"));
    auto* const passiveListFunction = find_function<UFunction>(
        STR("/Script/Pal.PalPassiveSkillManager:GetPalAssignablePassiveIDs"));
    if (manager != nullptr && passiveListFunction != nullptr) {
        /** @brief `GetPalAssignablePassiveIDs` 的反射输出布局。 */
        struct Params {
            TArray<FName> List; /**< 游戏写回的可分配被动技能 Raw ID 数组。 */
        } params;
        manager->ProcessEvent(passiveListFunction, &params);
        catalog.passive.skills.reserve(static_cast<std::size_t>(std::max(params.List.Num(), 0)));
        for (int32 index = 0; index < params.List.Num(); ++index) {
            const auto& id = params.List[index];
            catalog.passive.skills.push_back({.id = text_encoding::to_utf8(id.ToString()),
                                              .localizedName = passive_localized_name(
                                                  utility, passiveNameFunction, worldContext, id)});
        }
        catalog.passive.skills =
            skill_editor::deduplicate_skills(std::move(catalog.passive.skills));
    }

    catalog.active.skills = skill_editor::make_active_skill_options(
        skill_editor::active_skill_definitions(),
        [utility, activeNameFunction,
         worldContext](const skill_editor::ActiveSkillDefinition& definition) {
            return active_localized_name(utility, activeNameFunction, worldContext,
                                         static_cast<EPalWazaID>(definition.value));
        });

    const auto byLabel = [](const skill_editor::SkillOption& left,
                            const skill_editor::SkillOption& right) {
        return skill_editor::ascii_lower(skill_editor::skill_label(left)) <
               skill_editor::ascii_lower(skill_editor::skill_label(right));
    };

    if (catalog.passive.skills.empty()) {
        catalog.passive.error = "Unable to load Pal-assignable passive skills";
    } else {
        std::ranges::sort(catalog.passive.skills, byLabel);
        catalog.passive.ready = true;
    }

    if (catalog.active.skills.empty()) {
        catalog.active.error = "Generated EPalWazaID catalog is empty";
    } else {
        std::ranges::sort(catalog.active.skills, byLabel);
        catalog.active.ready = true;
    }

    const bool passiveHasLocalizedNames = std::ranges::any_of(
        catalog.passive.skills, [](const auto& option) { return !option.localizedName.empty(); });
    const bool activeHasLocalizedNames = std::ranges::any_of(
        catalog.active.skills, [](const auto& option) { return !option.localizedName.empty(); });
    const bool localizationContextReady = utility != nullptr && worldContext != nullptr;
    if (catalog.passive.ready && (!localizationContextReady || passiveNameFunction == nullptr ||
                                  !passiveHasLocalizedNames)) {
        catalog.passive.error = "Skill localization is unavailable; showing Raw IDs until refresh";
    }
    if (catalog.active.ready &&
        (!localizationContextReady || activeNameFunction == nullptr || !activeHasLocalizedNames)) {
        catalog.active.error = "Skill localization is unavailable; showing Raw IDs until refresh";
    }
    catalog.runtimeReady =
        manager != nullptr && passiveListFunction != nullptr && localizationContextReady &&
        passiveNameFunction != nullptr && activeNameFunction != nullptr && catalog.passive.ready &&
        catalog.active.ready && passiveHasLocalizedNames && activeHasLocalizedNames;
    return catalog;
}
}  // namespace pal_skills
