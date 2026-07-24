/**
 * @file pal_skills.cpp
 * @brief 实现 Palworld 技能编辑网关及运行时技能目录反射。
 * @details 本文件把无 Unreal 依赖的 `skill_editor` 领域服务映射到
 *          `PalIndividualCharacterParameter`、`PalPassiveSkillManager` 和
 *          `PalUIUtility`。所有接口均在游戏线程执行，所有 Unreal 裸指针均为非拥有观察指针。
 */
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
#include <skills/pal_skills.hpp>
#include <support/text_encoding.hpp>

using namespace RC;
using namespace RC::Unreal;

/** @brief 保存只供本翻译单元使用的 Palworld 反射辅助类型和函数。 */
namespace {
/**
 * @brief 与 Palworld 的 `EPalWazaID` 底层布局一致的强类型数值。
 * @details 枚举成员在运行时通过 Unreal `UEnum` 发现，本地只固定 16 位底层类型。
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
 * @param[in] worldContext 非拥有世界上下文对象。
 * @param[in] id 被动技能 Raw ID。
 * @return UTF-8 本地化名称；工具或反射函数不可用、文本转换失败时返回空字符串。
 */
[[nodiscard]] auto passive_localized_name(UObject* utility, UObject* worldContext, const FName& id)
    -> std::string {
    auto* function = find_function<UFunction>(STR("/Script/Pal.PalUIUtility:GetPassiveSkillName"));
    if (utility == nullptr || worldContext == nullptr || function == nullptr) {
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
 * @param[in] worldContext 非拥有世界上下文对象。
 * @param[in] id 主动技能 `EPalWazaID` 数值。
 * @return UTF-8 本地化名称；工具或反射函数不可用、文本转换失败时返回空字符串。
 */
[[nodiscard]] auto active_localized_name(UObject* utility, UObject* worldContext,
                                         const EPalWazaID id) -> std::string {
    auto* function = find_function<UFunction>(STR("/Script/Pal.PalUIUtility:GetWazaName"));
    if (utility == nullptr || worldContext == nullptr || function == nullptr) {
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

/**
 * @brief 去掉 Unreal 枚举项名称中的命名空间前缀。
 * @param[in] name 可能形如 `EPalWazaID::FireBall` 的 UTF-8 名称。
 * @return 只保留最后一个 `::` 之后内容的 Raw ID；无前缀时原样返回。
 */
[[nodiscard]] auto strip_enum_prefix(std::string name) -> std::string {
    if (const auto separator = name.rfind("::"); separator != std::string::npos) {
        name.erase(0, separator + 2);
    }
    return name;
}

/**
 * @brief 判断主动技能枚举项是否是不可分配的哨兵值。
 * @param[in] id 已移除枚举前缀的主动技能 Raw ID。
 * @retval true ID 为空、等于 `None`/`Max`（忽略 ASCII 大小写）或以 `_max` 结尾。
 * @retval false ID 表示候选业务技能。
 */
[[nodiscard]] auto is_active_sentinel(const std::string_view id) -> bool {
    const auto lowered = skill_editor::ascii_lower(id);
    return lowered.empty() || lowered == "none" || lowered == "max" || lowered.ends_with("_max");
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
            const auto found = activeIds_.find(value);
            state.activeSkills.push_back(
                {.value = value,
                 .id = found == activeIds_.end() ? std::to_string(value) : found->second});
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
 * @details 被动技能来自 `PalPassiveSkillManager:GetPalAssignablePassiveIDs`；主动技能来自
 *          运行时 `EPalWazaID` 枚举。结果会去重、过滤哨兵项、按本地化标签排序并重建
 *          activeIds_。完整调用契约见头文件中的成员声明。
 */
auto PalSkillGateway::load_catalog() -> skill_editor::SkillCatalogSnapshot {
    skill_editor::SkillCatalogSnapshot catalog;
    auto* const worldContext = pal_game::get_world_context();
    if (worldContext == nullptr) {
        catalog.error = "PalPlayerInventoryData world context is unavailable";
        return catalog;
    }
    auto* utility = ui_utility();

    auto* manager = UObjectGlobals::FindFirstOf(STR("PalPassiveSkillManager"));
    auto* passiveFunction = find_function<UFunction>(
        STR("/Script/Pal.PalPassiveSkillManager:GetPalAssignablePassiveIDs"));
    if (manager != nullptr && passiveFunction != nullptr) {
        /** @brief `GetPalAssignablePassiveIDs` 的反射输出布局。 */
        struct Params {
            TArray<FName> List; /**< 游戏写回的可分配被动技能 Raw ID 数组。 */
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
