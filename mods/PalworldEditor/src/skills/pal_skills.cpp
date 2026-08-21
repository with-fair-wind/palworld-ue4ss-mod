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
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/Core/Containers/Array.hpp>
#include <Unreal/Core/Containers/ScriptArray.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/FText.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/Property/FEnumProperty.hpp>
#include <Unreal/Property/FTextProperty.hpp>
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
    auto* const contextProperty = function == nullptr
                                      ? nullptr
                                      : CastField<FObjectPropertyBase>(function->FindProperty(
                                            FName(STR("WorldContextObject"), FNAME_Find)));
    auto* const idProperty = function == nullptr ? nullptr
                                                 : CastField<FNameProperty>(function->FindProperty(
                                                       FName(STR("PassiveSkillId"), FNAME_Find)));
    auto* const outNameProperty =
        function == nullptr
            ? nullptr
            : CastField<FTextProperty>(function->FindProperty(FName(STR("outName"), FNAME_Find)));
    if (!pal_game::is_valid(utility) || !pal_game::is_valid(worldContext) ||
        !pal_game::has_exact_parameter_count(function, 3) ||
        !pal_game::is_input_parameter(contextProperty) ||
        !pal_game::is_input_parameter(idProperty) ||
        !pal_game::is_output_parameter(outNameProperty)) {
        return {};
    }

    pal_game::FunctionParams params{function};
    contextProperty->SetObjectPropertyValue(
        contextProperty->ContainerPtrToValuePtr<void>(params.data()), worldContext);
    idProperty->SetPropertyValueInContainer(params.data(), id);
    utility->ProcessEvent(function, params.data());
    return text_encoding::to_utf8(
        outNameProperty->GetPropertyValueInContainer(params.data()).ToString());
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
    auto* const contextProperty = function == nullptr
                                      ? nullptr
                                      : CastField<FObjectPropertyBase>(function->FindProperty(
                                            FName(STR("WorldContextObject"), FNAME_Find)));
    auto* const idProperty =
        function == nullptr
            ? nullptr
            : CastField<FEnumProperty>(function->FindProperty(FName(STR("WazaID"), FNAME_Find)));
    auto* const idUnderlying =
        idProperty == nullptr ? nullptr : idProperty->GetUnderlyingProperty();
    auto* const outNameProperty =
        function == nullptr
            ? nullptr
            : CastField<FTextProperty>(function->FindProperty(FName(STR("outName"), FNAME_Find)));
    if (!pal_game::is_valid(utility) || !pal_game::is_valid(worldContext) ||
        !pal_game::has_exact_parameter_count(function, 3) ||
        !pal_game::is_input_parameter(contextProperty) ||
        !pal_game::is_input_parameter(idProperty) || idUnderlying == nullptr ||
        !idUnderlying->IsInteger() ||
        static_cast<std::size_t>(idUnderlying->GetElementSize()) != sizeof(EPalWazaID) ||
        !pal_game::is_output_parameter(outNameProperty)) {
        return {};
    }

    pal_game::FunctionParams params{function};
    contextProperty->SetObjectPropertyValue(
        contextProperty->ContainerPtrToValuePtr<void>(params.data()), worldContext);
    idUnderlying->SetIntPropertyValue(idProperty->ContainerPtrToValuePtr<void>(params.data()),
                                      static_cast<std::uint64_t>(static_cast<std::uint16_t>(id)));
    utility->ProcessEvent(function, params.data());
    return text_encoding::to_utf8(
        outNameProperty->GetPropertyValueInContainer(params.data()).ToString());
}

[[nodiscard]] auto get_waza_database(UObject* utility, UFunction* function, UObject* worldContext)
    -> UObject* {
    auto* const contextProperty = function == nullptr
                                      ? nullptr
                                      : CastField<FObjectPropertyBase>(function->FindProperty(
                                            FName(STR("WorldContextObject"), FNAME_Find)));
    auto* const returnProperty =
        function == nullptr ? nullptr
                            : CastField<FObjectPropertyBase>(function->GetReturnProperty());
    if (!pal_game::is_valid(utility) || !pal_game::is_valid(worldContext) ||
        !pal_game::has_exact_parameter_count(function, 2) ||
        !pal_game::is_input_parameter(contextProperty) ||
        !pal_game::is_return_parameter(returnProperty)) {
        return nullptr;
    }

    pal_game::FunctionParams params{function};
    contextProperty->SetObjectPropertyValue(
        contextProperty->ContainerPtrToValuePtr<void>(params.data()), worldContext);
    utility->ProcessEvent(function, params.data());
    auto* const result = returnProperty->GetObjectPropertyValue(
        returnProperty->ContainerPtrToValuePtr<void>(params.data()));
    return pal_game::is_valid(result) ? result : nullptr;
}

[[nodiscard]] auto read_active_category(UObject* wazaDatabase, UFunction* findWazaFunction,
                                        const std::uint16_t wazaValue)
    -> std::optional<skill_editor::ActiveSkillCategory> {
    if (wazaDatabase == nullptr || findWazaFunction == nullptr) {
        return std::nullopt;
    }
    auto* const typeProp = findWazaFunction->FindProperty(FName(STR("Type"), FNAME_Find));
    auto* const typeEnum = CastField<FEnumProperty>(typeProp);
    auto* const typeUnderlying = typeEnum == nullptr ? nullptr : typeEnum->GetUnderlyingProperty();
    auto* const returnProperty = CastField<FBoolProperty>(findWazaFunction->GetReturnProperty());
    auto* const outDataProp = CastField<FStructProperty>(
        findWazaFunction->FindProperty(FName(STR("OutData"), FNAME_Find)));
    auto* const outDataStruct = outDataProp == nullptr ? nullptr : outDataProp->GetStruct().Get();
    auto* const categoryProp = outDataStruct == nullptr
                                   ? nullptr
                                   : CastField<FEnumProperty>(outDataStruct->FindProperty(
                                         FName(STR("Category"), FNAME_Find)));
    auto* const categoryUnderlying =
        categoryProp == nullptr ? nullptr : categoryProp->GetUnderlyingProperty();
    if (!pal_game::has_exact_parameter_count(findWazaFunction, 3) ||
        !pal_game::is_input_parameter(typeEnum) || typeUnderlying == nullptr ||
        !typeUnderlying->IsInteger() || !pal_game::is_output_parameter(outDataProp) ||
        !pal_game::is_return_parameter(returnProperty) || categoryUnderlying == nullptr ||
        !categoryUnderlying->IsInteger()) {
        return std::nullopt;
    }

    pal_game::FunctionParams params{findWazaFunction};
    typeUnderlying->SetIntPropertyValue(typeEnum->ContainerPtrToValuePtr<void>(params.data()),
                                        static_cast<int64_t>(wazaValue));
    wazaDatabase->ProcessEvent(findWazaFunction, params.data());
    void* const outDataPtr = outDataProp->ContainerPtrToValuePtr<void>(params.data());
    const auto categoryValue = categoryUnderlying->GetSignedIntPropertyValue(
        categoryProp->ContainerPtrToValuePtr<void>(outDataPtr));
    return skill_editor::active_skill_category_from_lookup(
        returnProperty->GetPropertyValueInContainer(params.data()), categoryValue);
}

/** @brief 单只帕鲁的 `SaveParameter.MasteredWaza` 反射访问点。 */
struct MasteredWazaAccess {
    void* saveParameter{};
    FArrayProperty* arrayProperty{};
    FEnumProperty* elementProperty{};
    FNumericProperty* underlyingProperty{};
    UFunction* onRepSaveParameter{};
};

/** @brief 防止损坏数组元数据导致不受限分配或遍历。 */
inline constexpr int32 kMaxMasteredWazaCount = 1024;
/** @brief 防止技能返回数组损坏导致不受限分配或遍历。 */
inline constexpr int32 kMaxReturnedSkillCount = 64;

/** @brief 已通过运行时签名校验的主动技能写入函数与参数属性。 */
struct ActiveWriteFunctions {
    UFunction* clear{};
    UFunction* add{};
    FEnumProperty* wazaId{};
    FNumericProperty* underlying{};
};

[[nodiscard]] auto prepare_active_write_functions() -> std::optional<ActiveWriteFunctions> {
    auto* const clear =
        find_function<UFunction>(STR("/Script/Pal.PalIndividualCharacterParameter:ClearEquipWaza"));
    auto* const add =
        find_function<UFunction>(STR("/Script/Pal.PalIndividualCharacterParameter:AddEquipWaza"));
    auto* const wazaId =
        add == nullptr
            ? nullptr
            : CastField<FEnumProperty>(add->FindProperty(FName(STR("WazaId"), FNAME_Find)));
    auto* const underlying = wazaId == nullptr ? nullptr : wazaId->GetUnderlyingProperty();
    if (!pal_game::has_exact_parameter_count(clear, 0) || clear->GetReturnProperty() != nullptr ||
        !pal_game::has_exact_parameter_count(add, 1) || add->GetReturnProperty() != nullptr ||
        !pal_game::is_input_parameter(wazaId) || underlying == nullptr ||
        !underlying->IsInteger() ||
        static_cast<std::size_t>(underlying->GetElementSize()) != sizeof(EPalWazaID)) {
        return std::nullopt;
    }
    return ActiveWriteFunctions{
        .clear = clear,
        .add = add,
        .wazaId = wazaId,
        .underlying = underlying,
    };
}

[[nodiscard]] auto write_active_sequence(UObject* pal, const ActiveWriteFunctions& functions,
                                         const std::span<const skill_editor::ActiveSkill> sequence)
    -> bool {
    if (!pal_game::is_valid(pal)) {
        return false;
    }
    pal->ProcessEvent(functions.clear, nullptr);
    if (!pal_game::is_valid(pal)) {
        return false;
    }

    for (const auto& skill : sequence) {
        pal_game::FunctionParams params{functions.add};
        functions.underlying->SetIntPropertyValue(
            functions.wazaId->ContainerPtrToValuePtr<void>(params.data()),
            static_cast<std::uint64_t>(skill.value));
        pal->ProcessEvent(functions.add, params.data());
        if (!pal_game::is_valid(pal)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] auto prepare_mastered_waza(UObject* pal) -> std::optional<MasteredWazaAccess> {
    if (!pal_game::is_valid(pal)) {
        return std::nullopt;
    }
    auto* const saveProperty =
        CastField<FStructProperty>(pal->GetPropertyByNameInChain(STR("SaveParameter")));
    auto* const saveStruct = saveProperty == nullptr ? nullptr : saveProperty->GetStruct().Get();
    auto* const arrayProperty = saveStruct == nullptr
                                    ? nullptr
                                    : CastField<FArrayProperty>(saveStruct->FindProperty(
                                          FName(STR("MasteredWaza"), FNAME_Find)));
    auto* const elementProperty =
        arrayProperty == nullptr ? nullptr : CastField<FEnumProperty>(arrayProperty->GetInner());
    auto* const underlyingProperty =
        elementProperty == nullptr ? nullptr : elementProperty->GetUnderlyingProperty();
    void* const saveParameter =
        saveProperty == nullptr ? nullptr : saveProperty->ContainerPtrToValuePtr<void>(pal);
    if (saveParameter == nullptr || arrayProperty == nullptr || elementProperty == nullptr ||
        underlyingProperty == nullptr || !underlyingProperty->IsInteger() ||
        static_cast<std::size_t>(underlyingProperty->GetElementSize()) != sizeof(EPalWazaID) ||
        !!(arrayProperty->GetArrayFlags() & EArrayPropertyFlags::UsesMemoryImageAllocator)) {
        return std::nullopt;
    }
    return MasteredWazaAccess{
        .saveParameter = saveParameter,
        .arrayProperty = arrayProperty,
        .elementProperty = elementProperty,
        .underlyingProperty = underlyingProperty,
        .onRepSaveParameter = pal->GetFunctionByNameInChain(STR("OnRep_SaveParameter")),
    };
}

[[nodiscard]] auto read_mastered_waza(const MasteredWazaAccess& access)
    -> std::optional<std::vector<std::uint16_t>> {
    FScriptArrayHelper_InContainer values{access.arrayProperty, access.saveParameter};
    const auto count = values.Num();
    if (count < 0 || count > kMaxMasteredWazaCount) {
        return std::nullopt;
    }
    std::vector<std::uint16_t> result;
    result.reserve(static_cast<std::size_t>(count));
    for (int32 index{}; index < count; ++index) {
        const auto value = access.underlyingProperty->GetUnsignedIntPropertyValue(
            access.elementProperty->ContainerPtrToValuePtr<void>(values.GetRawPtr(index)));
        if (value > std::numeric_limits<std::uint16_t>::max()) {
            return std::nullopt;
        }
        result.push_back(static_cast<std::uint16_t>(value));
    }
    return result;
}

[[nodiscard]] auto append_mastered_waza(const MasteredWazaAccess& access,
                                        const std::span<const std::uint16_t> values) -> bool {
    const auto current = read_mastered_waza(access);
    if (!current.has_value() ||
        current->size() + values.size() > static_cast<std::size_t>(kMaxMasteredWazaCount)) {
        return false;
    }
    // 写路径不能走 FScriptArrayHelper::AddValues：其 freezable 数组分支引用
    // FMemoryImageAllocatorBase::ResizeAllocation，本 UE4SS 构建未导出该符号，无法链接。
    // 这里按 property 的元素大小/对齐直接调用 FScriptArray::Add + InitializeValue，
    // 与 helper 对堆数组（Palworld 可编辑数组均为堆数组）的执行路径一致。
    auto* const inner = access.arrayProperty->GetInner();
    auto* const array =
        access.arrayProperty->ContainerPtrToValuePtr<FScriptArray>(access.saveParameter);
    if (inner == nullptr || array == nullptr) {
        return false;
    }
    const auto elementSize = inner->GetElementSize();
    const auto elementAlignment = inner->GetMinAlignment();
    for (const auto value : values) {
        const auto addedIndex = array->Add(1, elementSize, elementAlignment);
        auto* const element = static_cast<std::uint8_t*>(array->GetData()) +
                              static_cast<std::size_t>(addedIndex) * elementSize;
        inner->InitializeValue(element);
        access.underlyingProperty->SetIntPropertyValue(
            access.elementProperty->ContainerPtrToValuePtr<void>(element),
            static_cast<std::uint64_t>(value));
    }
    return true;
}

[[nodiscard]] auto restore_mastered_waza_tail(const MasteredWazaAccess& access,
                                              const std::span<const std::uint16_t> original)
    -> bool {
    const auto current = read_mastered_waza(access);
    if (!current.has_value() || current->size() < original.size() ||
        !std::ranges::equal(original, std::span{*current}.first(original.size()))) {
        return false;
    }

    // 同 append_mastered_waza：FScriptArrayHelper::RemoveValues 的 freezable 分支在本
    // UE4SS 构建无法链接，按 property 大小/对齐直接调用 FScriptArray::Remove。
    auto* const inner = access.arrayProperty->GetInner();
    auto* const array =
        access.arrayProperty->ContainerPtrToValuePtr<FScriptArray>(access.saveParameter);
    if (inner == nullptr || array == nullptr) {
        return false;
    }
    const auto elementSize = inner->GetElementSize();
    const auto elementAlignment = inner->GetMinAlignment();
    const auto firstRemoved = static_cast<int32>(original.size());
    const auto removedCount = array->Num() - firstRemoved;
    for (int32 index = array->Num(); index > firstRemoved; --index) {
        auto* const element = static_cast<std::uint8_t*>(array->GetData()) +
                              static_cast<std::size_t>(index - 1) * elementSize;
        inner->DestroyValue(element);
    }
    array->Remove(firstRemoved, removedCount, elementSize, elementAlignment);
    return read_mastered_waza(access) ==
           std::optional<std::vector<std::uint16_t>>{
               std::vector<std::uint16_t>{original.begin(), original.end()}};
}

[[nodiscard]] auto notify_mastered_waza_changed(UObject* pal, const MasteredWazaAccess& access)
    -> bool {
    if (!pal_game::is_valid(pal) ||
        !pal_game::has_exact_parameter_count(access.onRepSaveParameter, 0) ||
        access.onRepSaveParameter->GetReturnProperty() != nullptr) {
        return false;
    }
    pal->ProcessEvent(access.onRepSaveParameter, nullptr);
    return true;
}

[[nodiscard]] auto same_active_values(const std::span<const skill_editor::ActiveSkill> left,
                                      const std::span<const skill_editor::ActiveSkill> right)
    -> bool {
    return left.size() == right.size() &&
           std::ranges::equal(left, right, [](const auto& lhs, const auto& rhs) {
               return lhs.value == rhs.value;
           });
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
    -> skill_editor::SkillStateReadResult {
    skill_editor::SkillStateReadResult result;
    auto* pal = to_pal(target);
    if (pal == nullptr) {
        return result;
    }

    auto* const passiveFunction = find_function<UFunction>(
        STR("/Script/Pal.PalIndividualCharacterParameter:GetPassiveSkillList"));
    auto* const passiveArray =
        passiveFunction == nullptr
            ? nullptr
            : CastField<FArrayProperty>(passiveFunction->GetReturnProperty());
    auto* const passiveElement =
        passiveArray == nullptr ? nullptr : CastField<FNameProperty>(passiveArray->GetInner());
    if (pal_game::has_exact_parameter_count(passiveFunction, 1) &&
        pal_game::is_return_parameter(passiveArray) && passiveElement != nullptr &&
        !(!!(passiveArray->GetArrayFlags() & EArrayPropertyFlags::UsesMemoryImageAllocator))) {
        pal_game::FunctionParams params{passiveFunction};
        pal->ProcessEvent(passiveFunction, params.data());
        FScriptArrayHelper_InContainer values{passiveArray, params.data()};
        const auto count = values.Num();
        if (count >= 0 && count <= kMaxReturnedSkillCount) {
            result.state.passiveIds.reserve(static_cast<std::size_t>(count));
            for (int32 index{}; index < count; ++index) {
                FName id;
                passiveElement->CopyCompleteValue(&id, values.GetRawPtr(index));
                result.state.passiveIds.push_back(text_encoding::to_utf8(id.ToString()));
            }
            result.passiveReadable = true;
        }
    }

    auto* const activeFunction =
        find_function<UFunction>(STR("/Script/Pal.PalIndividualCharacterParameter:GetEquipWaza"));
    auto* const activeArray = activeFunction == nullptr
                                  ? nullptr
                                  : CastField<FArrayProperty>(activeFunction->GetReturnProperty());
    auto* const activeElement =
        activeArray == nullptr ? nullptr : CastField<FEnumProperty>(activeArray->GetInner());
    auto* const activeUnderlying =
        activeElement == nullptr ? nullptr : activeElement->GetUnderlyingProperty();
    pal = to_pal(target);
    if (pal != nullptr && pal_game::has_exact_parameter_count(activeFunction, 1) &&
        pal_game::is_return_parameter(activeArray) && activeElement != nullptr &&
        activeUnderlying != nullptr && activeUnderlying->IsInteger() &&
        !(!!(activeArray->GetArrayFlags() & EArrayPropertyFlags::UsesMemoryImageAllocator))) {
        pal_game::FunctionParams params{activeFunction};
        pal->ProcessEvent(activeFunction, params.data());
        FScriptArrayHelper_InContainer values{activeArray, params.data()};
        const auto returnedCount = values.Num();
        if (returnedCount >= 0 && returnedCount <= kMaxReturnedSkillCount) {
            const auto editableCount = std::min<int32>(returnedCount, 3);
            result.state.activeSkills.reserve(static_cast<std::size_t>(editableCount));
            bool valuesValid = true;
            for (int32 index{}; index < editableCount; ++index) {
                const auto raw = activeUnderlying->GetUnsignedIntPropertyValue(
                    activeElement->ContainerPtrToValuePtr<void>(values.GetRawPtr(index)));
                if (raw > std::numeric_limits<std::uint16_t>::max()) {
                    valuesValid = false;
                    break;
                }
                const auto value = static_cast<std::uint16_t>(raw);
                result.state.activeSkills.push_back(
                    {.value = value, .id = skill_editor::active_skill_id_or_numeric(value)});
            }
            if (valuesValid) {
                result.activeReadable = true;
            } else {
                result.state.activeSkills.clear();
            }
            if (returnedCount > 3) {
                Output::send<LogLevel::Warning>(
                    STR("PalworldEditor: GetEquipWaza returned {} entries; only the first 3 are "
                        "editable\n"),
                    returnedCount);
            }
        }
    }

    return result;
}

/** @details 运行时验证两个 FName 输入属性；OverrideSkill 明确传 NAME_None。 */
auto PalSkillGateway::add_passive(const skill_editor::SkillTarget target, const std::string_view id)
    -> bool {
    auto* pal = to_pal(target);
    auto* function = find_function<UFunction>(
        STR("/Script/Pal.PalIndividualCharacterParameter:AddPassiveSkill"));
    auto* const addProperty =
        function == nullptr
            ? nullptr
            : CastField<FNameProperty>(function->FindProperty(FName(STR("AddSkill"), FNAME_Find)));
    auto* const overrideProperty =
        function == nullptr ? nullptr
                            : CastField<FNameProperty>(
                                  function->FindProperty(FName(STR("OverrideSkill"), FNAME_Find)));
    if (pal == nullptr || id.empty() || !pal_game::has_exact_parameter_count(function, 2) ||
        !pal_game::is_input_parameter(addProperty) ||
        !pal_game::is_input_parameter(overrideProperty)) {
        return false;
    }

    const auto wide = text_encoding::widen_ascii(id);
    const FName addSkill{wide.c_str()};
    const FName overrideSkill{};
    pal_game::FunctionParams params{function};
    addProperty->SetPropertyValueInContainer(params.data(), addSkill);
    overrideProperty->SetPropertyValueInContainer(params.data(), overrideSkill);
    pal->ProcessEvent(function, params.data());
    return true;
}

/** @details 运行时验证 SkillId 为唯一的 FName 输入参数后再调用。 */
auto PalSkillGateway::remove_passive(const skill_editor::SkillTarget target,
                                     const std::string_view id) -> bool {
    auto* pal = to_pal(target);
    auto* function = find_function<UFunction>(
        STR("/Script/Pal.PalIndividualCharacterParameter:RemovePassiveSkill"));
    auto* const idProperty =
        function == nullptr
            ? nullptr
            : CastField<FNameProperty>(function->FindProperty(FName(STR("SkillId"), FNAME_Find)));
    if (pal == nullptr || id.empty() || !pal_game::has_exact_parameter_count(function, 1) ||
        !pal_game::is_input_parameter(idProperty)) {
        return false;
    }

    const auto wide = text_encoding::widen_ascii(id);
    const FName skillId{wide.c_str()};
    pal_game::FunctionParams params{function};
    idProperty->SetPropertyValueInContainer(params.data(), skillId);
    pal->ProcessEvent(function, params.data());
    return true;
}

/**
 * @details 先把缺失技能追加到 `SaveParameter.MasteredWaza` 并通知重读，再重写
 *          `EquipWaza`。任一步失败都会在当前调用内恢复原掌握列表和装备序列。
 */
auto PalSkillGateway::rewrite_active(const skill_editor::SkillTarget target,
                                     const std::span<const skill_editor::ActiveSkill> skills)
    -> skill_editor::ActiveWriteResult {
    auto* pal = to_pal(target);
    auto originalRead = read_state(target);
    if (pal == nullptr || skills.size() > 3 || !originalRead.activeReadable) {
        return {
            .status = skill_editor::ActiveWriteStatus::preflightFailed,
            .readback = std::move(originalRead),
        };
    }

    pal = to_pal(target);
    const auto functions = prepare_active_write_functions();
    const auto masteredAccess = prepare_mastered_waza(pal);
    const auto originalMastered =
        masteredAccess.has_value() ? read_mastered_waza(*masteredAccess) : std::nullopt;
    if (pal == nullptr || !functions.has_value() || !masteredAccess.has_value() ||
        !originalMastered.has_value()) {
        return {
            .status = skill_editor::ActiveWriteStatus::preflightFailed,
            .readback = std::move(originalRead),
        };
    }

    const auto originalActive = originalRead.state.activeSkills;

    auto desiredMastered = *originalMastered;
    std::vector<std::uint16_t> newlyMastered;
    for (const auto& skill : skills) {
        if (!std::ranges::contains(desiredMastered, skill.value)) {
            desiredMastered.push_back(skill.value);
            newlyMastered.push_back(skill.value);
        }
    }
    const bool masteredChanged = !newlyMastered.empty();
    const auto restore = [&]() -> skill_editor::ActiveWriteResult {
        bool masteredRestored = !masteredChanged;
        pal = to_pal(target);
        if (masteredChanged) {
            const auto restoreAccess = prepare_mastered_waza(pal);
            masteredRestored = restoreAccess.has_value() &&
                               restore_mastered_waza_tail(*restoreAccess, *originalMastered) &&
                               notify_mastered_waza_changed(pal, *restoreAccess);
        }

        pal = to_pal(target);
        const auto restoreFunctions = prepare_active_write_functions();
        const bool activeRestored = pal != nullptr && restoreFunctions.has_value() &&
                                    write_active_sequence(pal, *restoreFunctions, originalActive);

        auto restoredRead = read_state(target);
        pal = to_pal(target);
        const auto verifiedAccess = prepare_mastered_waza(pal);
        const auto restoredMastered =
            verifiedAccess.has_value() ? read_mastered_waza(*verifiedAccess) : std::nullopt;
        const bool verified = masteredRestored && activeRestored &&
                              restoredMastered == originalMastered && restoredRead.activeReadable &&
                              same_active_values(restoredRead.state.activeSkills, originalActive);
        return {
            .status = verified ? skill_editor::ActiveWriteStatus::rolledBack
                               : skill_editor::ActiveWriteStatus::rollbackFailed,
            .readback = std::move(restoredRead),
        };
    };

    if (masteredChanged && !append_mastered_waza(*masteredAccess, newlyMastered)) {
        return {
            .status = skill_editor::ActiveWriteStatus::preflightFailed,
            .readback = std::move(originalRead),
        };
    }

    if (masteredChanged) {
        if (!notify_mastered_waza_changed(pal, *masteredAccess)) {
            return restore();
        }
        pal = to_pal(target);
        const auto verifiedAccess = prepare_mastered_waza(pal);
        if (!verifiedAccess.has_value() ||
            read_mastered_waza(*verifiedAccess) !=
                std::optional<std::vector<std::uint16_t>>{desiredMastered}) {
            return restore();
        }
    }

    pal = to_pal(target);
    const auto writeFunctions = prepare_active_write_functions();
    if (pal == nullptr || !writeFunctions.has_value() ||
        !write_active_sequence(pal, *writeFunctions, skills)) {
        return restore();
    }

    auto actualRead = read_state(target);
    pal = to_pal(target);
    const auto actualAccess = prepare_mastered_waza(pal);
    const auto actualMastered =
        actualAccess.has_value() ? read_mastered_waza(*actualAccess) : std::nullopt;
    const bool masteredVerified =
        actualMastered == std::optional<std::vector<std::uint16_t>>{desiredMastered};
    if (actualRead.activeReadable && same_active_values(actualRead.state.activeSkills, skills) &&
        masteredVerified) {
        return {
            .status = skill_editor::ActiveWriteStatus::succeeded,
            .readback = std::move(actualRead),
        };
    }

    return restore();
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
    } else if (!pal_game::has_exact_parameter_count(function, 3) ||
               !pal_game::is_input_parameter(skillNameProperty) ||
               !pal_game::is_output_parameter(outSkillDataProperty) ||
               !pal_game::is_return_parameter(returnProperty)) {
        result.error = "GetSkillData signature does not match Palworld 1.0 metadata";
    }
    if (!result.error.empty()) {
        finish();
        return result;
    }

    const auto count = std::min(ids.size(), maxItems);
    result.entries.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        {
            pal_game::FunctionParams params{function};

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
    auto* const passiveList = passiveListFunction == nullptr
                                  ? nullptr
                                  : CastField<FArrayProperty>(passiveListFunction->FindProperty(
                                        FName(STR("List"), FNAME_Find)));
    auto* const passiveId =
        passiveList == nullptr ? nullptr : CastField<FNameProperty>(passiveList->GetInner());
    if (pal_game::is_valid(manager) &&
        pal_game::has_exact_parameter_count(passiveListFunction, 1) &&
        pal_game::is_output_parameter(passiveList) && passiveId != nullptr) {
        pal_game::FunctionParams params{passiveListFunction};
        manager->ProcessEvent(passiveListFunction, params.data());
        FScriptArrayHelper_InContainer values{passiveList, params.data()};
        const int32 skillCount = values.Num();
        if (skillCount < 0 || skillCount > 10'000) {
            catalog.passive.error = "GetPalAssignablePassiveIDs returned an invalid array size";
        } else {
            catalog.passive.skills.reserve(static_cast<std::size_t>(skillCount));
        }
        for (int32 index = 0; index < skillCount && catalog.passive.error.empty(); ++index) {
            FName id{};
            passiveId->CopyCompleteValue(&id, values.GetRawPtr(index));
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

    // 读取每个主动技能的 Category（Melee/Shot/Support）。
    auto* const palUtility = UObjectGlobals::StaticFindObject<UObject*>(
        nullptr, nullptr, STR("/Script/Pal.Default__PalUtility"));
    auto* const getWazaDbFunction =
        find_function<UFunction>(STR("/Script/Pal.PalUtility:GetWazaDatabase"));
    auto* const wazaDatabase = get_waza_database(palUtility, getWazaDbFunction, worldContext);
    auto* const findWazaFunction =
        pal_game::is_valid(wazaDatabase)
            ? wazaDatabase->GetFunctionByNameInChain(STR("FindWazaForBP"))
            : nullptr;
    if (findWazaFunction != nullptr) {
        for (auto& skill : catalog.active.skills) {
            if (skill.activeValue.has_value()) {
                skill.activeCategory =
                    read_active_category(wazaDatabase, findWazaFunction, *skill.activeValue);
            }
        }
    }

    const auto byLabel = [](const skill_editor::SkillOption& left,
                            const skill_editor::SkillOption& right) {
        return skill_editor::ascii_lower(skill_editor::skill_label(left)) <
               skill_editor::ascii_lower(skill_editor::skill_label(right));
    };

    if (catalog.passive.skills.empty()) {
        if (catalog.passive.error.empty()) {
            catalog.passive.error = "Unable to load Pal-assignable passive skills";
        }
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
