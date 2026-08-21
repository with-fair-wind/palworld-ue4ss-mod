/**
 * @file pal_game.hpp
 * @brief 提供 PalworldEditor 对背包、物品、帕鲁对象和诊断扫描的游戏反射适配接口。
 * @details 本文件中的函数通过 UE4SS 访问 Unreal UObject 和 `ProcessEvent`。除纯常量外，
 *          所有接口都必须在 Unreal 初始化完成后的游戏线程调用；返回的 Unreal 裸指针均为
 *          非拥有观察指针，不会延长游戏对象生命周期。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/Core/Containers/Map.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/FString.hpp>
#include <Unreal/FText.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/Property/FEnumProperty.hpp>
#include <Unreal/Property/FTextProperty.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UnrealCoreStructs.hpp>
#include <common/game_reflection.hpp>
#include <common/text_encoding.hpp>
#include <items/item_catalog.hpp>
#include <skills/selected_target_state.hpp>

using namespace RC;
using namespace RC::Unreal;

/** @brief 封装 PalworldEditor 直接调用的游戏线程反射操作。 */
namespace pal_game {
/** @brief 本地玩家队伍 Holder 的运行时解析结果与诊断信息。 */
struct LocalOtomoHolderResolution {
    UObject* holder{}; /**< 唯一的本地玩家 Holder；解析失败时为空。 */
    skill_editor::SelectedTargetResolutionStatus status{
        skill_editor::SelectedTargetResolutionStatus::holderCandidatesUnavailable};
    std::size_t candidateCount{};      /**< 有效 Holder 候选数量。 */
    std::size_t localCandidateCount{}; /**< 由本地控制器拥有的 Holder 数量。 */
    std::wstring candidateClasses;     /**< 用于状态变化日志的候选实际类名。 */
};

/**
 * @brief 从所有 Otomo Holder 中解析唯一属于本地玩家队伍的实例。
 * @return Holder 观察指针、分阶段状态和候选诊断；不会选择第一个候选作为回退。
 * @warning 只能在游戏线程调用，返回的 Holder 不能跨帧缓存。
 */
[[nodiscard]] inline auto resolve_local_otomo_holder() -> LocalOtomoHolderResolution {
    std::vector<UObject*> holders;
    UObjectGlobals::FindAllOf(STR("PalOtomoHolderComponentBase"), holders);

    std::wstring candidateClasses;
    for (auto* const holder : holders) {
        if (!is_valid(holder)) {
            continue;
        }
        if (!candidateClasses.empty()) {
            candidateClasses.append(STR(", "));
        }
        candidateClasses.append(holder->GetClassPrivate()->GetName());
    }

    const auto selection = skill_editor::find_unique_local_candidate(
        holders, [](UObject* holder) { return is_valid(holder); },
        [](UObject* holder) {
            return invoke<UObject*>(holder, STR("TryGetOwnerControlledPawn")).value_or(nullptr);
        },
        [](UObject* pawn) {
            return invoke<UObject*>(pawn, STR("GetController")).value_or(nullptr);
        },
        [](UObject* controller) {
            return invoke<bool>(controller, STR("IsLocalPlayerController")).value_or(false);
        });

    using SelectionStatus = skill_editor::LocalCandidateSelectionStatus;
    using ResolutionStatus = skill_editor::SelectedTargetResolutionStatus;
    const auto status = [&] {
        switch (selection.status) {
            case SelectionStatus::success:
                return ResolutionStatus::success;
            case SelectionStatus::noCandidates:
                return ResolutionStatus::holderCandidatesUnavailable;
            case SelectionStatus::ownerPawnUnavailable:
                return ResolutionStatus::holderOwnerPawnUnavailable;
            case SelectionStatus::ownerControllerUnavailable:
                return ResolutionStatus::holderOwnerControllerUnavailable;
            case SelectionStatus::localCandidateUnavailable:
                return ResolutionStatus::localHolderUnavailable;
            case SelectionStatus::ambiguousLocalCandidates:
                return ResolutionStatus::localHolderAmbiguous;
        }
        return ResolutionStatus::localHolderUnavailable;
    }();

    return {
        .holder = selection.candidate.value_or(nullptr),
        .status = status,
        .candidateCount = selection.candidateCount,
        .localCandidateCount = selection.localCandidateCount,
        .candidateClasses = std::move(candidateClasses),
    };
}

/**
 * @brief 按队伍槽位读取个体 Handle，并在调用前验证运行时参数属性。
 * @param[in] holder 当前帧解析到的本地队伍 Holder。
 * @param[in] slotIndex 要查询的槽位索引。
 * @param[out] handle 游戏返回的非拥有 Handle；空槽位时为 nullptr。
 * @retval true 函数签名可验证且反射调用已经执行。
 * @retval false 目标、槽位或函数参数元数据不符合预期，未执行调用。
 */
[[nodiscard]] inline auto try_get_otomo_individual_handle(UObject* holder, const int32 slotIndex,
                                                          UObject*& handle) -> bool {
    handle = nullptr;
    if (!is_valid(holder) || slotIndex < 0) {
        return false;
    }

    auto* const function = holder->GetFunctionByNameInChain(STR("GetOtomoIndividualHandle"));
    auto* const slotProperty =
        function == nullptr
            ? nullptr
            : CastField<FIntProperty>(function->FindProperty(FName(STR("SlotIndex"), FNAME_Find)));
    auto* const returnProperty =
        function == nullptr ? nullptr
                            : CastField<FObjectPropertyBase>(function->GetReturnProperty());
    std::size_t parameterCount{};
    if (function != nullptr) {
        for (auto* property :
             TFieldRange<FProperty>(function, EFieldIterationFlags::IncludeDeprecated)) {
            if (property->HasAnyPropertyFlags(CPF_Parm)) {
                ++parameterCount;
            }
        }
    }
    if (parameterCount != 2 || slotProperty == nullptr || returnProperty == nullptr ||
        !slotProperty->HasAnyPropertyFlags(CPF_Parm) ||
        slotProperty->HasAnyPropertyFlags(CPF_OutParm | CPF_ReturnParm) ||
        !returnProperty->HasAnyPropertyFlags(CPF_Parm) ||
        !returnProperty->HasAnyPropertyFlags(CPF_ReturnParm)) {
        return false;
    }

    FunctionParams params{function};
    slotProperty->SetPropertyValueInContainer(params.data(), slotIndex);
    holder->ProcessEvent(function, params.data());
    auto* const result = returnProperty->GetObjectPropertyValue(
        returnProperty->ContainerPtrToValuePtr<void>(params.data()));
    handle = is_valid(result) ? result : nullptr;
    return true;
}

/**
 * @brief 当前待出战帕鲁的运行时解析结果。
 */
struct SelectedPalTarget {
    /** @brief 当前帕鲁的个体参数对象；解析失败时为空。 */
    UObject* parameter{};

    /** @brief Holder 是否提供了权威的当前出战 Handle 查询接口。 */
    bool spawnStateKnown{};

    /** @brief 当前选中 Handle 是否就是 Holder 报告的当前出战 Handle。 */
    bool selectedIsSpawned{};

    /** @brief 跨线程发布所需的纯值个体 GUID 与 CharacterID。 */
    skill_editor::SelectedTargetObservation observation;

    /** @brief 当前反射解析链的终止状态。 */
    skill_editor::SelectedTargetResolutionStatus status{
        skill_editor::SelectedTargetResolutionStatus::holderCandidatesUnavailable};

    /** @brief 当前帧发现的有效 Holder 候选数量。 */
    std::size_t holderCandidateCount{};

    /** @brief 当前帧发现的本地玩家 Holder 候选数量。 */
    std::size_t localHolderCandidateCount{};

    /** @brief 当前帧 Holder 候选的实际类名，用于状态变化日志。 */
    std::wstring holderCandidateClasses;
};

/**
 * @brief 解析数字键当前高亮、下一次按 E 召唤的队伍帕鲁。
 * @return 当前帧参数对象、纯值个体身份和分步解析状态。
 * @details 从唯一的本地玩家 Otomo Holder 开始，依次取得当前高亮槽位、个体 handle、
 *          个体 parameter、`FPalInstanceID.InstanceId` 和 CharacterID。
 * @warning 只能在游戏线程调用；返回的参数对象只允许在当前帧使用。
 */
[[nodiscard]] inline auto resolve_selected_otomo() -> SelectedPalTarget {
    using enum skill_editor::SelectedTargetResolutionStatus;
    auto holderResolution = resolve_local_otomo_holder();
    const auto failure =
        [&holderResolution](const skill_editor::SelectedTargetResolutionStatus status) {
            return SelectedPalTarget{
                .status = status,
                .holderCandidateCount = holderResolution.candidateCount,
                .localHolderCandidateCount = holderResolution.localCandidateCount,
                .holderCandidateClasses = std::move(holderResolution.candidateClasses),
            };
        };

    auto* const holder = holderResolution.holder;
    if (!is_valid(holder)) {
        return failure(holderResolution.status);
    }

    const auto selectedSlot = invoke<int32>(holder, STR("GetSelectedOtomoID"));
    if (!selectedSlot.has_value()) {
        return failure(getSelectedFunctionUnavailable);
    }
    if (*selectedSlot < 0) {
        return failure(selectedSlotUnavailable);
    }

    UObject* handle{};
    if (!try_get_otomo_individual_handle(holder, *selectedSlot, handle)) {
        return failure(getHandleFunctionUnavailable);
    }
    if (!is_valid(handle)) {
        return failure(handleUnavailable);
    }

    auto* const getSpawnedHandleFunction =
        holder->GetFunctionByNameInChain(STR("TryGetSpawnedOtomoHandle"));
    auto* const spawnedHandleResult =
        getSpawnedHandleFunction == nullptr
            ? nullptr
            : CastField<FObjectPropertyBase>(getSpawnedHandleFunction->GetReturnProperty());
    UObject* spawnedHandle{};
    const bool spawnStateKnown = has_exact_parameter_count(getSpawnedHandleFunction, 1) &&
                                 is_return_parameter(spawnedHandleResult);
    if (spawnStateKnown) {
        FunctionParams params{getSpawnedHandleFunction};
        holder->ProcessEvent(getSpawnedHandleFunction, params.data());
        spawnedHandle = spawnedHandleResult->GetObjectPropertyValue(
            spawnedHandleResult->ContainerPtrToValuePtr<void>(params.data()));
    }

    auto* const getParameterFunction =
        handle->GetFunctionByNameInChain(STR("TryGetIndividualParameter"));
    auto* const parameterResult =
        getParameterFunction == nullptr
            ? nullptr
            : CastField<FObjectPropertyBase>(getParameterFunction->GetReturnProperty());
    if (!has_exact_parameter_count(getParameterFunction, 1) ||
        !is_return_parameter(parameterResult)) {
        return failure(getParameterFunctionUnavailable);
    }
    FunctionParams parameterParams{getParameterFunction};
    handle->ProcessEvent(getParameterFunction, parameterParams.data());
    auto* const parameter = parameterResult->GetObjectPropertyValue(
        parameterResult->ContainerPtrToValuePtr<void>(parameterParams.data()));
    if (!is_valid(parameter)) {
        return failure(parameterUnavailable);
    }

    auto* const expectedClass = UObjectGlobals::StaticFindObject<UClass*>(
        nullptr, nullptr, STR("/Script/Pal.PalIndividualCharacterParameter"));
    if (expectedClass == nullptr || !parameter->GetClassPrivate()->IsChildOf(expectedClass)) {
        return failure(parameterClassUnavailable);
    }

    auto* const getPalIdFunction = parameter->GetFunctionByNameInChain(STR("GetPalId"));
    auto* const palIdResult =
        getPalIdFunction == nullptr
            ? nullptr
            : CastField<FStructProperty>(getPalIdFunction->GetReturnProperty());
    auto* const instanceIdProperty =
        palIdResult == nullptr || palIdResult->GetStruct() == nullptr
            ? nullptr
            : CastField<FStructProperty>(
                  palIdResult->GetStruct()->FindProperty(FName(STR("InstanceId"), FNAME_Find)));
    if (!has_exact_parameter_count(getPalIdFunction, 1) || !is_return_parameter(palIdResult) ||
        !matches_struct_identity(instanceIdProperty, STR("Guid"), sizeof(FGuid))) {
        return failure(getPalIdFunctionUnavailable);
    }
    FunctionParams palIdParams{getPalIdFunction};
    parameter->ProcessEvent(getPalIdFunction, palIdParams.data());
    auto* const palId = palIdResult->ContainerPtrToValuePtr<void>(palIdParams.data());
    FGuid instanceId{};
    instanceIdProperty->CopyCompleteValue(&instanceId,
                                          instanceIdProperty->ContainerPtrToValuePtr<void>(palId));
    if (!instanceId.is_valid()) {
        return failure(individualIdUnavailable);
    }

    const auto characterId = invoke<FName>(parameter, STR("GetCharacterID"));
    if (!characterId.has_value()) {
        return failure(getCharacterIdFunctionUnavailable);
    }

    return {
        .parameter = parameter,
        .spawnStateKnown = spawnStateKnown,
        .selectedIsSpawned = spawnStateKnown && spawnedHandle == handle,
        .observation =
            {
                .identity =
                    {
                        .instanceId = {instanceId.A, instanceId.B, instanceId.C, instanceId.D},
                    },
                .name = text_encoding::to_utf8(characterId->ToString()),
            },
        .status = success,
        .holderCandidateCount = holderResolution.candidateCount,
        .localHolderCandidateCount = holderResolution.localCandidateCount,
        .holderCandidateClasses = std::move(holderResolution.candidateClasses),
    };
}

/**
 * @brief UObject 诊断扫描关注的类名关键字。
 * @details 用于缩小诊断日志范围，不参与物品、背包或技能的业务扫描。
 */
inline constexpr std::wstring_view kDiscoveryKeywords[] = {
    L"Inventory", L"IndividualCharacter", L"ItemContainer", L"Otomo", L"PalCharacterContainer",
};
/** @brief Palworld 物品主数据读取的防御性领域上限。 */
inline constexpr int32 kMaximumStaticItemEntries = 100'000;
/** @brief 稀疏 Map 最大索引的防御性上限，避免异常元数据造成无界遍历。 */
inline constexpr int32 kMaximumStaticItemMapIndex = 200'000;
/** @brief 主背包槽位读取的防御性上限，避免异常 Num() 驱动无界 ProcessEvent。 */
inline constexpr int32 kMaximumInventorySlotCount = 10'000;

/** @brief 表示主背包中的一个非空物品槽快照。 */
struct InvEntry {
    std::string item_id; /**< 传给游戏接口的物品 Raw ID，不是本地化展示名称。 */
    int count;           /**< 扫描时读取到的堆叠数量。 */
    int32_t slot_index;  /**< 容器槽位索引；修改数量时使用此值，而不是 `item_id`。 */
};

/** @brief 主背包槽位数量直接写事务的可验证结果。 */
enum class SlotCountWriteStatus : std::uint8_t {
    succeeded,       /**< 写入值已经同步重读确认。 */
    preflightFailed, /**< 容器、槽位、属性或输入范围在写前不可用。 */
    rolledBack,      /**< 写后验证失败，但原值已经恢复并重读确认。 */
    rollbackFailed,  /**< 写后验证失败，且无法确认原值恢复。 */
};

/**
 * @brief 获取主玩家的 Common 背包容器。
 * @return 指向主背包容器的非拥有观察指针。
 * @retval nullptr 背包对象、反射函数或对应容器不可用。
 * @warning 只能在游戏线程调用，返回值跨帧使用前需要重新校验。
 */
inline auto get_main_container() -> UObject* {
    UObject* inv = UObjectGlobals::FindFirstOf(kInventoryClassName);
    if (inv == nullptr) {
        return nullptr;
    }
    UFunction* fn = UObjectGlobals::StaticFindObject<UFunction*>(
        nullptr, nullptr,
        STR("/Script/Pal.PalPlayerInventoryData:TryGetContainerFromInventoryType"));
    if (fn == nullptr) {
        return nullptr;
    }
    auto* const type =
        CastField<FEnumProperty>(fn->FindProperty(FName(STR("inventoryType"), FNAME_Find)));
    auto* const typeUnderlying = type == nullptr ? nullptr : type->GetUnderlyingProperty();
    auto* const output =
        CastField<FObjectPropertyBase>(fn->FindProperty(FName(STR("OutContainer"), FNAME_Find)));
    auto* const result = CastField<FBoolProperty>(fn->GetReturnProperty());
    if (!has_exact_parameter_count(fn, 3) || !is_input_parameter(type) ||
        typeUnderlying == nullptr || !typeUnderlying->IsInteger() || !is_output_parameter(output) ||
        !is_return_parameter(result)) {
        return nullptr;
    }
    FunctionParams params{fn};
    typeUnderlying->SetIntPropertyValue(type->ContainerPtrToValuePtr<void>(params.data()),
                                        std::uint64_t{0});
    inv->ProcessEvent(fn, params.data());
    if (!result->GetPropertyValueInContainer(params.data())) {
        return nullptr;
    }
    auto* const container =
        output->GetObjectPropertyValue(output->ContainerPtrToValuePtr<void>(params.data()));
    return is_valid(container) ? container : nullptr;
}

/**
 * @brief 读取物品容器的槽位总数。
 * @param[in] container 非拥有物品容器指针。
 * @return 容器报告的槽位数量。
 * @retval 0 容器为空、`PalItemContainer:Num` 不可用或容器本身为空。
 * @warning 只能在游戏线程调用。
 */
inline auto container_num(UObject* container) -> int32_t {
    return invoke<int32>(container, STR("Num")).value_or(0);
}

/**
 * @brief 按索引获取物品容器中的槽位对象。
 * @param[in] container 非拥有物品容器指针。
 * @param[in] index 要读取的槽位索引，调用方应保证其位于 `[0, container_num())`。
 * @return 指向槽位对象的非拥有观察指针。
 * @retval nullptr 容器、反射函数或对应槽位不可用。
 * @warning 只能在游戏线程调用。
 */
inline auto container_get(UObject* container, int32_t index) -> UObject* {
    if (!is_valid(container) || index < 0) {
        return nullptr;
    }
    auto* const function = container->GetFunctionByNameInChain(STR("Get"));
    auto* const input =
        function == nullptr
            ? nullptr
            : CastField<FIntProperty>(function->FindProperty(FName(STR("Index"), FNAME_Find)));
    auto* const result = function == nullptr
                             ? nullptr
                             : CastField<FObjectPropertyBase>(function->GetReturnProperty());
    if (!has_exact_parameter_count(function, 2) || !is_input_parameter(input) ||
        !is_return_parameter(result)) {
        return nullptr;
    }
    FunctionParams params{function};
    input->SetPropertyValueInContainer(params.data(), index);
    container->ProcessEvent(function, params.data());
    auto* const slot =
        result->GetObjectPropertyValue(result->ContainerPtrToValuePtr<void>(params.data()));
    return is_valid(slot) ? slot : nullptr;
}

/**
 * @brief 通过 `StackCount` 属性读取一个物品槽的堆叠数量。
 * @param[in] slot 非拥有物品槽指针。
 * @return 槽位当前的堆叠数量。
 * @retval 0 槽位为空、属性不存在或属性不是 `FIntProperty`。
 * @warning 只能在游戏线程调用。
 */
inline auto read_slot_stack_count(UObject* slot) -> int32_t {
    if (slot == nullptr) {
        return 0;
    }
    FProperty* sc = slot->GetPropertyByNameInChain(STR("StackCount"));
    if (auto* ip = CastField<FIntProperty>(sc)) {
        return ip->GetPropertyValueInContainer(slot);
    }
    return 0;
}

/**
 * @brief 扫描主背包并生成全部非空物品槽快照。
 * @return 按容器槽位顺序排列的非空物品列表。
 * @note 只保留 `StackCount > 0` 且 `ItemId` 能转换为非空 UTF-8 Raw ID 的槽位。
 * @warning 只能在游戏线程调用；返回结果不持有任何 UObject 指针。
 */
inline auto read_inventory() -> std::vector<InvEntry> {
    std::vector<InvEntry> items;
    UObject* container = get_main_container();
    if (container == nullptr) {
        Output::send<LogLevel::Warning>(
            STR("read_inventory: container not found (not in-game?)\n"));
        return items;
    }
    const int32_t num = container_num(container);
    if (num < 0 || num > kMaximumInventorySlotCount) {
        Output::send<LogLevel::Warning>(
            STR("read_inventory: slot count {} is outside the safe domain.\n"), num);
        return items;
    }
    int nonEmpty = 0;
    for (int32_t i = 0; i < num; ++i) {
        UObject* slot = container_get(container, i);
        if (slot == nullptr) {
            continue;
        }
        const int32_t count = read_slot_stack_count(slot);
        std::string name;
        // ItemId 是 FPalItemId 结构（首成员 StaticId FName），不是 FName 属性：
        // 必须经结构内字段类型化读取，禁止把容器指针直接当 FName 解引用。
        // 不做 matches_struct_identity 名称校验：UStruct 名（PalItemId）未经 UHT dump
        // 证实，猜错会导致物品目录全空；字段级类型校验已保证 fail-closed。
        if (FStructProperty* itemIdProp =
                CastField<FStructProperty>(slot->GetPropertyByNameInChain(STR("ItemId")))) {
            if (UStruct* itemIdStruct = itemIdProp->GetStruct().Get()) {
                if (FNameProperty* staticIdProp = CastField<FNameProperty>(
                        itemIdStruct->FindProperty(FName(STR("StaticId"), FNAME_Find)))) {
                    if (const FName* sid = staticIdProp->ContainerPtrToValuePtr<FName>(
                            itemIdProp->ContainerPtrToValuePtr<void>(slot))) {
                        name = text_encoding::to_utf8(sid->ToString());
                    }
                }
            }
        }
        if (count > 0 && !name.empty()) {
            items.push_back({name, static_cast<int>(count), i});
            ++nonEmpty;
        }
    }
    Output::send<LogLevel::Warning>(STR("read_inventory: {} slots, {} non-empty items\n"), num,
                                    nonEmpty);
    return items;
}

/**
 * @brief 直接修改主背包指定槽位的 `StackCount` 属性。
 * @param[in] slotIndex 主背包容器槽位索引。
 * @param[in] newCount 要写入的数量；本接口不执行范围裁剪。
 * @return 写入、验证及必要回滚的明确结果。
 * @warning 只能在游戏线程调用；没有已验证的原生 setter/OnRep，因此仍属于本地直接属性写。
 */
[[nodiscard]] inline auto set_slot_count(const int32_t slotIndex, const int32_t newCount)
    -> SlotCountWriteStatus {
    if (slotIndex < 0 || slotIndex >= kMaximumInventorySlotCount || newCount < 0 ||
        newCount > 9999) {
        return SlotCountWriteStatus::preflightFailed;
    }
    UObject* container = get_main_container();
    if (container == nullptr) {
        return SlotCountWriteStatus::preflightFailed;
    }
    UObject* slot = container_get(container, slotIndex);
    if (slot == nullptr) {
        return SlotCountWriteStatus::preflightFailed;
    }
    FProperty* sc = slot->GetPropertyByNameInChain(STR("StackCount"));
    auto* ip = CastField<FIntProperty>(sc);
    if (ip == nullptr) {
        return SlotCountWriteStatus::preflightFailed;
    }
    const int32_t old = ip->GetPropertyValueInContainer(slot);
    if (old == newCount) {
        return SlotCountWriteStatus::succeeded;
    }
    ip->SetPropertyValueInContainer(slot, newCount);
    if (ip->GetPropertyValueInContainer(slot) == newCount) {
        Output::send<LogLevel::Warning>(STR("set_slot_count: slot {} StackCount {} -> {}\n"),
                                        slotIndex, old, newCount);
        return SlotCountWriteStatus::succeeded;
    }

    ip->SetPropertyValueInContainer(slot, old);
    if (ip->GetPropertyValueInContainer(slot) == old) {
        Output::send<LogLevel::Warning>(
            STR("set_slot_count: write verification failed for slot {}; original restored.\n"),
            slotIndex);
        return SlotCountWriteStatus::rolledBack;
    }
    Output::send<LogLevel::Error>(
        STR("set_slot_count: write and rollback verification failed for slot {}.\n"), slotIndex);
    return SlotCountWriteStatus::rollbackFailed;
}

/**
 * @brief 通过 `AddItem_ServerInternal` 向玩家主背包添加物品。
 * @param[in] itemId 仅含 ASCII 的物品 Raw ID；本接口使用 widen_ascii() 构造 `FName`。
 * @param[in] count 直接传给游戏函数的添加数量。
 * @warning 只能在游戏线程调用。背包对象或反射函数不可用时不执行添加。
 */
inline auto give_items(const std::string& itemId, int32 count) -> void {
    UObject* inventory = UObjectGlobals::FindFirstOf(kInventoryClassName);
    if (inventory == nullptr) {
        Output::send<LogLevel::Warning>(STR("give_items: inventory not found\n"));
        return;
    }
    UFunction* fn = UObjectGlobals::StaticFindObject<UFunction*>(
        nullptr, nullptr, STR("/Script/Pal.PalPlayerInventoryData:AddItem_ServerInternal"));
    if (fn == nullptr) {
        return;
    }
    const std::wstring wide = text_encoding::widen_ascii(itemId);
    auto* const id =
        CastField<FNameProperty>(fn->FindProperty(FName(STR("StaticItemId"), FNAME_Find)));
    auto* const itemCount =
        CastField<FIntProperty>(fn->FindProperty(FName(STR("Count"), FNAME_Find)));
    auto* const assignPassive =
        CastField<FBoolProperty>(fn->FindProperty(FName(STR("IsAssignPassive"), FNAME_Find)));
    auto* const logDelay =
        CastField<FFloatProperty>(fn->FindProperty(FName(STR("LogDelay"), FNAME_Find)));
    auto* const notifyLog =
        CastField<FBoolProperty>(fn->FindProperty(FName(STR("bNotifyLog"), FNAME_Find)));
    auto* const result = CastField<FEnumProperty>(fn->GetReturnProperty());
    auto* const resultUnderlying = result == nullptr ? nullptr : result->GetUnderlyingProperty();
    if (!has_exact_parameter_count(fn, 6) || !is_input_parameter(id) ||
        !is_input_parameter(itemCount) || !is_input_parameter(assignPassive) ||
        !is_input_parameter(logDelay) || !is_input_parameter(notifyLog) ||
        !is_return_parameter(result) || resultUnderlying == nullptr ||
        !resultUnderlying->IsInteger()) {
        Output::send<LogLevel::Warning>(
            STR("give_items: AddItem_ServerInternal signature unavailable\n"));
        return;
    }
    FunctionParams params{fn};
    id->SetPropertyValueInContainer(params.data(), FName(wide.c_str()));
    itemCount->SetPropertyValueInContainer(params.data(), count);
    assignPassive->SetPropertyValueInContainer(params.data(), false);
    logDelay->SetPropertyValueInContainer(params.data(), 0.0F);
    notifyLog->SetPropertyValueInContainer(params.data(), false);
    inventory->ProcessEvent(fn, params.data());
    const auto operationResult = resultUnderlying->GetUnsignedIntPropertyValue(
        result->ContainerPtrToValuePtr<void>(params.data()));
    Output::send<LogLevel::Warning>(
        STR("give_items: AddItem_ServerInternal('{}', x{}) -> result={}\n"), wide, count,
        operationResult);
}

/**
 * @brief 查找提供物品本地化名称的 `PalUIUtility` 默认对象。
 * @return 指向 UI 工具对象的非拥有观察指针。
 * @retval nullptr 默认对象和当前已加载对象中均未找到 `PalUIUtility`。
 * @note 优先使用 `/Script/Pal.Default__PalUIUtility`，再退回按类名查找。
 * @warning 只能在游戏线程调用。
 */
inline auto get_ui_utility() -> UObject* {
    if (auto* utility = UObjectGlobals::StaticFindObject<UObject*>(
            nullptr, nullptr, STR("/Script/Pal.Default__PalUIUtility"))) {
        return utility;
    }
    return UObjectGlobals::FindFirstOf(STR("PalUIUtility"));
}

/**
 * @brief 调用 `PalUIUtility:GetItemName` 获取指定物品的当前语言名称。
 * @param[in] utility 非拥有 `PalUIUtility` 对象指针。
 * @param[in] function 非拥有 `GetItemName` 反射函数指针。
 * @param[in] worldContext 非拥有世界上下文对象，通常使用主背包数据对象。
 * @param[in] id 物品的 `FName` Raw ID。
 * @return 转换为 UTF-8 的当前游戏语言名称。
 * @retval std::string{} 任一必需对象为空，或返回的 `FText` 无法转换。
 * @warning 所有 Unreal 指针只在游戏对象仍有效期间可用，且本接口只能在游戏线程调用。
 */
inline auto localized_item_name(UObject* utility, UFunction* function, UObject* worldContext,
                                const FName& id) -> std::string {
    auto* const context = function == nullptr
                              ? nullptr
                              : CastField<FObjectPropertyBase>(function->FindProperty(
                                    FName(STR("WorldContextObject"), FNAME_Find)));
    auto* const itemId = function == nullptr ? nullptr
                                             : CastField<FNameProperty>(function->FindProperty(
                                                   FName(STR("StaticItemId"), FNAME_Find)));
    auto* const name =
        function == nullptr
            ? nullptr
            : CastField<FTextProperty>(function->FindProperty(FName(STR("outName"), FNAME_Find)));
    if (!is_valid(utility) || !is_valid(worldContext) || !has_exact_parameter_count(function, 3) ||
        !is_input_parameter(context) || !is_input_parameter(itemId) || !is_output_parameter(name)) {
        return {};
    }
    FunctionParams params{function};
    context->SetObjectPropertyValue(context->ContainerPtrToValuePtr<void>(params.data()),
                                    worldContext);
    itemId->SetPropertyValueInContainer(params.data(), id);
    utility->ProcessEvent(function, params.data());
    const auto* const localized = name->ContainerPtrToValuePtr<FText>(params.data());
    return localized == nullptr ? std::string{} : text_encoding::to_utf8(localized->ToString());
}

/**
 * @brief 从当前世界的 PalItemIDManager 解析权威静态物品数据资产。
 * @param[in] worldContext 非拥有世界上下文对象。
 * @return PalStaticItemDataAsset 非拥有观察指针；任一反射入口尚未就绪时为空。
 * @warning 只能在游戏线程调用。
 */
[[nodiscard]] inline auto get_static_item_data_asset(UObject* worldContext) -> UObject* {
    if (worldContext == nullptr) {
        return nullptr;
    }
    auto* utility = UObjectGlobals::StaticFindObject<UObject*>(
        nullptr, nullptr, STR("/Script/Pal.Default__PalUtility"));
    auto* function = UObjectGlobals::StaticFindObject<UFunction*>(
        nullptr, nullptr, STR("/Script/Pal.PalUtility:GetItemIDManager"));
    if (utility == nullptr || function == nullptr) {
        return nullptr;
    }

    auto* const context = CastField<FObjectPropertyBase>(
        function->FindProperty(FName(STR("WorldContextObject"), FNAME_Find)));
    auto* const result = CastField<FObjectPropertyBase>(function->GetReturnProperty());
    if (!has_exact_parameter_count(function, 2) || !is_input_parameter(context) ||
        !is_return_parameter(result)) {
        return nullptr;
    }
    FunctionParams params{function};
    context->SetObjectPropertyValue(context->ContainerPtrToValuePtr<void>(params.data()),
                                    worldContext);
    utility->ProcessEvent(function, params.data());
    auto* manager =
        result->GetObjectPropertyValue(result->ContainerPtrToValuePtr<void>(params.data()));
    if (!is_valid(manager)) {
        return nullptr;
    }

    auto* assetProperty = CastField<FObjectPropertyBase>(
        manager->GetPropertyByNameInChain(STR("StaticItemDataAsset")));
    auto** asset = assetProperty == nullptr
                       ? nullptr
                       : assetProperty->ContainerPtrToValuePtr<UObject*>(manager);
    return asset != nullptr && is_valid(*asset) ? *asset : nullptr;
}

/**
 * @brief 从 PalStaticItemDataAsset.StaticItemDataMap 枚举全部物品 Raw ID。
 * @param[in] dataAsset 非拥有静态物品数据资产。
 * @param[out] ids 收集到的 Raw ID；仅在成功解析权威 Map 时写入。
 * @return 成功识别 TMap<FName, PalStaticItemDataBase*> 布局时为 true。
 * @warning 只能在游戏线程调用，且不得跨帧保存 Map 地址。
 */
[[nodiscard]] inline auto collect_static_item_ids(UObject* dataAsset, std::vector<FName>& ids)
    -> bool {
    auto* mapProperty = dataAsset == nullptr
                            ? nullptr
                            : CastField<FMapProperty>(
                                  dataAsset->GetPropertyByNameInChain(STR("StaticItemDataMap")));
    auto* keyProperty =
        mapProperty == nullptr ? nullptr : CastField<FNameProperty>(mapProperty->GetKeyProp());
    auto* valueProperty = mapProperty == nullptr
                              ? nullptr
                              : CastField<FObjectPropertyBase>(mapProperty->GetValueProp());
    auto* map = mapProperty == nullptr ? nullptr
                                       : mapProperty->ContainerPtrToValuePtr<FScriptMap>(dataAsset);
    if (mapProperty == nullptr || keyProperty == nullptr || valueProperty == nullptr ||
        map == nullptr) {
        return false;
    }

    const int32 entryCount = map->Num();
    const int32 maximumIndex = map->GetMaxIndex();
    if (entryCount <= 0 || entryCount > kMaximumStaticItemEntries || maximumIndex < 0 ||
        maximumIndex > kMaximumStaticItemMapIndex) {
        return false;
    }

    const auto layout =
        FScriptMap::GetScriptLayout(keyProperty->GetSize(), keyProperty->GetMinAlignment(),
                                    valueProperty->GetSize(), valueProperty->GetMinAlignment());
    std::vector<FName> discovered;
    discovered.reserve(static_cast<std::size_t>(entryCount));
    for (int32 index{}; index < maximumIndex; ++index) {
        if (!map->IsValidIndex(index)) {
            continue;
        }
        auto* entry = map->GetData(index, layout);
        auto* id = keyProperty->ContainerPtrToValuePtr<FName>(entry);
        if (id != nullptr && !id->ToString().empty()) {
            discovered.push_back(*id);
        }
    }
    if (discovered.empty()) {
        return false;
    }
    ids = std::move(discovered);
    return true;
}

/** @brief 一次物品目录扫描的纯值结果和数据来源状态。 */
struct ItemCatalogScanResult {
    item_catalog::ItemCatalogSnapshot catalog;
    bool usedStaticItemDataMap{};
};

/**
 * @brief 扫描物品主数据并建立可搜索的本地化目录。
 * @return 已去重、排序并建立 Raw ID 标签索引的物品目录快照。
 * @details 优先从 PalItemIDManager 到 StaticItemDataAsset 再到 StaticItemDataMap
 *          枚举权威 Raw ID。仅当世界主数据尚未就绪时，才回退扫描当前已加载的
 *          PalStaticItemData UObject。
 * @note 名称解析失败时目录标签回退到 Raw ID；回退目录仍可能不完整。
 * @warning 只能在游戏线程调用。
 */
inline auto scan_all_items() -> ItemCatalogScanResult {
    std::vector<item_catalog::ItemOption> items;
    auto* utility = get_ui_utility();
    auto* function = UObjectGlobals::StaticFindObject<UFunction*>(
        nullptr, nullptr, STR("/Script/Pal.PalUIUtility:GetItemName"));
    auto* worldContext = UObjectGlobals::FindFirstOf(kInventoryClassName);

    std::vector<FName> ids;
    const bool usedMasterData =
        collect_static_item_ids(get_static_item_data_asset(worldContext), ids);
    if (usedMasterData) {
        items.reserve(ids.size());
        for (const auto& id : ids) {
            items.push_back(
                {.id = text_encoding::to_utf8(id.ToString()),
                 .localizedName = localized_item_name(utility, function, worldContext, id)});
        }
        auto catalog = item_catalog::make_item_catalog(std::move(items));
        Output::send<LogLevel::Warning>(
            STR("scan_all_items: source=StaticItemDataMap, mapEntries={}, catalog={}\n"),
            static_cast<int32>(ids.size()), static_cast<int32>(catalog.items.size()));
        return {.catalog = std::move(catalog), .usedStaticItemDataMap = true};
    }

    int32 matchedClass{};
    int32 passedFilter{};
    int32 withId{};
    UObjectGlobals::ForEachUObject([&](UObject* obj, int32_t, int32_t) -> LoopAction {
        UClass* cls = obj->GetClassPrivate();
        if (cls == nullptr) {
            return LoopAction::Continue;
        }
        const std::wstring name = cls->GetName();
        if (name.find(L"PalStaticItemData") != 0) {
            return LoopAction::Continue;
        }
        ++matchedClass;
        if (name.find(L"Table") != std::wstring::npos ||
            name.find(L"Asset") != std::wstring::npos ||
            name.find(L"Manager") != std::wstring::npos ||
            name.find(L"Struct") != std::wstring::npos ||
            name.find(L"AndNum") != std::wstring::npos ||
            name.find(L"RowName") != std::wstring::npos) {
            return LoopAction::Continue;
        }
        ++passedFilter;
        FProperty* idProperty = obj->GetPropertyByNameInChain(STR("ID"));
        // Raw ID 可能是 FName 属性或 FPalItemId 结构（首成员 StaticId FName）：
        // 两种布局都经属性类型化读取，类型不匹配时跳过（fail-closed）。
        // 不做结构名称校验：UStruct 名未经 dump 证实，字段级校验已保证 fail-closed。
        const FName* id = nullptr;
        if (FNameProperty* idName = CastField<FNameProperty>(idProperty)) {
            id = idName->ContainerPtrToValuePtr<FName>(obj);
        } else if (FStructProperty* idStruct = CastField<FStructProperty>(idProperty)) {
            if (UStruct* idStructType = idStruct->GetStruct().Get()) {
                if (FNameProperty* staticIdName = CastField<FNameProperty>(
                        idStructType->FindProperty(FName(STR("StaticId"), FNAME_Find)))) {
                    id = staticIdName->ContainerPtrToValuePtr<FName>(
                        idStruct->ContainerPtrToValuePtr<void>(obj));
                }
            }
        }
        if (id != nullptr) {
            const std::wstring rawId = id->ToString();
            if (!rawId.empty()) {
                ++withId;
                items.push_back(
                    {.id = text_encoding::to_utf8(rawId),
                     .localizedName = localized_item_name(utility, function, worldContext, *id)});
            }
        }
        return LoopAction::Continue;
    });
    auto catalog = item_catalog::make_item_catalog(std::move(items));
    Output::send<LogLevel::Warning>(
        STR("scan_all_items: source=loaded UObject fallback, classMatch={}, passedFilter={}, "
            "withId={}, catalog={}\n"),
        matchedClass, passedFilter, withId, static_cast<int32>(catalog.items.size()));
    return {.catalog = std::move(catalog), .usedStaticItemDataMap = false};
}

/**
 * @brief 输出与 PalworldEditor 关注对象相关的 UObject 类名直方图。
 * @details 扫描全部已加载 UObject，只统计类名包含 kDiscoveryKeywords 任一关键字的对象，
 *          并按类名最多输出 200 条计数记录。
 * @note 本接口只写诊断日志，不修改游戏对象。
 * @warning 只能在游戏线程调用。
 */
inline auto discover_objects() -> void {
    Output::send<LogLevel::Warning>(STR("=== PalworldEditor discovery: scanning UObjects ===\n"));
    std::map<std::wstring, int> matching;
    int total = 0;
    UObjectGlobals::ForEachUObject([&](UObject* obj, int32_t, int32_t) -> LoopAction {
        ++total;
        UClass* cls = obj->GetClassPrivate();
        if (cls == nullptr) {
            return LoopAction::Continue;
        }
        std::wstring name = cls->GetName();
        for (auto kw : kDiscoveryKeywords) {
            if (name.find(kw) != std::wstring::npos) {
                ++matching[name];
                break;
            }
        }
        return LoopAction::Continue;
    });
    Output::send<LogLevel::Warning>(
        STR("=== discovery: {} total objects, {} matching class types ===\n"), total,
        matching.size());
    int n = 0;
    for (const auto& [clsName, cnt] : matching) {
        Output::send<LogLevel::Warning>(STR("[discover] {} (x{})\n"), clsName, cnt);
        if (++n >= 200) {
            break;
        }
    }
    Output::send<LogLevel::Warning>(STR("=== discovery done ===\n"));
}

}  // namespace pal_game
