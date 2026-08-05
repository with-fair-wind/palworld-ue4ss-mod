/**
 * @file pal_game.hpp
 * @brief 提供 PalworldEditor 对背包、物品、帕鲁对象和诊断扫描的游戏反射适配接口。
 * @details 本文件中的函数通过 UE4SS 访问 Unreal UObject 和 `ProcessEvent`。除纯常量外，
 *          所有接口都必须在 Unreal 初始化完成后的游戏线程调用；返回的 Unreal 裸指针均为
 *          非拥有观察指针，不会延长游戏对象生命周期。
 */
#pragma once

#include <cstddef>
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

    auto* const getSelectedFunction = holder->GetFunctionByNameInChain(STR("GetSelectedOtomoID"));
    if (getSelectedFunction == nullptr) {
        return failure(getSelectedFunctionUnavailable);
    }
    /** @brief `PalOtomoHolderComponentBase:GetSelectedOtomoID` 的返回参数布局。 */
    struct GetSelectedParams {
        int32_t ReturnValue{-1}; /**< 游戏写回的当前选中 Otomo 槽位索引。 */
    } getSelectedParams;
    holder->ProcessEvent(getSelectedFunction, &getSelectedParams);
    if (getSelectedParams.ReturnValue < 0) {
        return failure(selectedSlotUnavailable);
    }

    auto* const getHandleFunction = UObjectGlobals::StaticFindObject<UFunction*>(
        nullptr, nullptr, STR("/Script/Pal.PalOtomoHolderComponentBase:GetOtomoIndividualHandle"));
    if (getHandleFunction == nullptr) {
        return failure(getHandleFunctionUnavailable);
    }
    /** @brief `PalOtomoHolderComponentBase:GetOtomoIndividualHandle` 的反射参数布局。 */
    struct GetHandleParams {
        int32_t SlotIndex{};    /**< 要解析的当前选中槽位。 */
        UObject* ReturnValue{}; /**< 游戏写回的非拥有个体 handle。 */
    } getHandleParams{.SlotIndex = getSelectedParams.ReturnValue};
    holder->ProcessEvent(getHandleFunction, &getHandleParams);
    auto* const handle = getHandleParams.ReturnValue;
    if (!is_valid(handle)) {
        return failure(handleUnavailable);
    }

    auto* const getSpawnedHandleFunction =
        holder->GetFunctionByNameInChain(STR("TryGetSpawnedOtomoHandle"));
    auto* const spawnedHandleResult =
        getSpawnedHandleFunction == nullptr
            ? nullptr
            : CastField<FObjectPropertyBase>(
                  getSpawnedHandleFunction->FindProperty(FName(STR("ReturnValue"), FNAME_Find)));
    UObject* spawnedHandle{};
    const bool spawnStateKnown = spawnedHandleResult != nullptr;
    if (spawnStateKnown) {
        std::vector<std::byte> params(
            static_cast<std::size_t>(getSpawnedHandleFunction->GetParmsSize()));
        getSpawnedHandleFunction->InitializeStruct(params.data());
        holder->ProcessEvent(getSpawnedHandleFunction, params.data());
        spawnedHandle = spawnedHandleResult->GetObjectPropertyValue(
            spawnedHandleResult->ContainerPtrToValuePtr<void>(params.data()));
        getSpawnedHandleFunction->DestroyStruct(params.data());
    }

    auto* const getParameterFunction = UObjectGlobals::StaticFindObject<UFunction*>(
        nullptr, nullptr,
        STR("/Script/Pal.PalIndividualCharacterHandle:TryGetIndividualParameter"));
    if (getParameterFunction == nullptr) {
        return failure(getParameterFunctionUnavailable);
    }
    /** @brief `PalIndividualCharacterHandle:TryGetIndividualParameter` 的返回参数布局。 */
    struct GetParameterParams {
        UObject* ReturnValue{}; /**< 游戏写回的非拥有个体参数对象。 */
    } getParameterParams;
    handle->ProcessEvent(getParameterFunction, &getParameterParams);
    auto* const parameter = getParameterParams.ReturnValue;
    if (!is_valid(parameter)) {
        return failure(parameterUnavailable);
    }

    auto* const expectedClass = UObjectGlobals::StaticFindObject<UClass*>(
        nullptr, nullptr, STR("/Script/Pal.PalIndividualCharacterParameter"));
    if (expectedClass == nullptr || !parameter->GetClassPrivate()->IsChildOf(expectedClass)) {
        return failure(parameterClassUnavailable);
    }

    auto* const getPalIdFunction = UObjectGlobals::StaticFindObject<UFunction*>(
        nullptr, nullptr, STR("/Script/Pal.PalIndividualCharacterParameter:GetPalId"));
    if (getPalIdFunction == nullptr) {
        return failure(getPalIdFunctionUnavailable);
    }
    /** @brief 与 Palworld `FPalInstanceID` 的 UHT 字段顺序一致的返回值布局。 */
    struct PalInstanceId {
        FGuid PlayerUId;   /**< 帕鲁所属玩家的 GUID。 */
        FGuid InstanceId;  /**< 唯一标识该帕鲁个体的 GUID。 */
        FString DebugName; /**< 游戏内部调试名称；本 mod 不使用。 */
    };
    /** @brief `PalIndividualCharacterParameter:GetPalId` 的反射返回布局。 */
    struct GetPalIdParams {
        PalInstanceId ReturnValue; /**< 游戏写回的完整个体 ID。 */
    } getPalIdParams;
    parameter->ProcessEvent(getPalIdFunction, &getPalIdParams);
    const auto& instanceId = getPalIdParams.ReturnValue.InstanceId;
    if (!instanceId.is_valid()) {
        return failure(individualIdUnavailable);
    }

    auto* const getCharacterIdFunction = UObjectGlobals::StaticFindObject<UFunction*>(
        nullptr, nullptr, STR("/Script/Pal.PalIndividualCharacterParameter:GetCharacterID"));
    if (getCharacterIdFunction == nullptr) {
        return failure(getCharacterIdFunctionUnavailable);
    }
    /** @brief `PalIndividualCharacterParameter:GetCharacterID` 的返回参数布局。 */
    struct GetCharacterIdParams {
        FName ReturnValue; /**< 游戏写回的帕鲁 CharacterID。 */
    } getCharacterIdParams;
    parameter->ProcessEvent(getCharacterIdFunction, &getCharacterIdParams);

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
                .name = text_encoding::to_utf8(getCharacterIdParams.ReturnValue.ToString()),
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

/** @brief 表示主背包中的一个非空物品槽快照。 */
struct InvEntry {
    std::string item_id; /**< 传给游戏接口的物品 Raw ID，不是本地化展示名称。 */
    int count;           /**< 扫描时读取到的堆叠数量。 */
    int32_t slot_index;  /**< 容器槽位索引；修改数量时使用此值，而不是 `item_id`。 */
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
    /** @brief `TryGetContainerFromInventoryType` 的反射参数布局。 */
    struct {
        uint8_t Type; /**< `EPalPlayerInventoryType` 数值；0 表示 Common。 */
        UObject* Out; /**< 游戏写回的非拥有容器指针。 */
        bool Ret;     /**< 游戏函数写回的成功标志；当前实现以 `Out` 为准。 */
    } p{};
    p.Type = 0;  // EPalPlayerInventoryType::Common
    inv->ProcessEvent(fn, &p);
    return p.Out;
}

/**
 * @brief 读取物品容器的槽位总数。
 * @param[in] container 非拥有物品容器指针。
 * @return 容器报告的槽位数量。
 * @retval 0 容器为空、`PalItemContainer:Num` 不可用或容器本身为空。
 * @warning 只能在游戏线程调用。
 */
inline auto container_num(UObject* container) -> int32_t {
    UFunction* fn = UObjectGlobals::StaticFindObject<UFunction*>(
        nullptr, nullptr, STR("/Script/Pal.PalItemContainer:Num"));
    if (fn == nullptr || container == nullptr) {
        return 0;
    }
    /** @brief `PalItemContainer:Num` 的返回参数布局。 */
    struct {
        int32_t Ret; /**< 游戏函数写回的槽位数量。 */
    } n{};
    container->ProcessEvent(fn, &n);
    return n.Ret;
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
    UFunction* fn = UObjectGlobals::StaticFindObject<UFunction*>(
        nullptr, nullptr, STR("/Script/Pal.PalItemContainer:Get"));
    if (fn == nullptr || container == nullptr) {
        return nullptr;
    }
    /** @brief `PalItemContainer:Get` 的反射参数布局。 */
    struct {
        int32_t Index; /**< 传入游戏函数的槽位索引。 */
        UObject* Slot; /**< 游戏函数写回的非拥有槽位指针。 */
    } gp{};
    gp.Index = index;
    container->ProcessEvent(fn, &gp);
    return gp.Slot;
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
    int nonEmpty = 0;
    for (int32_t i = 0; i < num; ++i) {
        UObject* slot = container_get(container, i);
        if (slot == nullptr) {
            continue;
        }
        const int32_t count = read_slot_stack_count(slot);
        std::string name;
        if (FProperty* itemIdProp = slot->GetPropertyByNameInChain(STR("ItemId"))) {
            if (FName* sid = itemIdProp->ContainerPtrToValuePtr<FName>(slot)) {
                const std::wstring w = sid->ToString();
                name = text_encoding::to_utf8(w);
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
 * @warning 只能在游戏线程调用。容器、槽位或属性不可用时静默返回。
 */
inline auto set_slot_count(int32_t slotIndex, int32_t newCount) -> void {
    UObject* container = get_main_container();
    if (container == nullptr) {
        return;
    }
    UObject* slot = container_get(container, slotIndex);
    if (slot == nullptr) {
        return;
    }
    FProperty* sc = slot->GetPropertyByNameInChain(STR("StackCount"));
    auto* ip = CastField<FIntProperty>(sc);
    if (ip == nullptr) {
        return;
    }
    const int32_t old = ip->GetPropertyValueInContainer(slot);
    ip->SetPropertyValueInContainer(slot, newCount);
    Output::send<LogLevel::Warning>(STR("set_slot_count: slot {} StackCount {} -> {}\n"), slotIndex,
                                    old, newCount);
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
    /** @brief `AddItem_ServerInternal` 的反射参数布局。 */
    struct {
        FName StaticItemId;   /**< 要添加的物品 Raw ID。 */
        int32_t Count;        /**< 要添加的物品数量。 */
        bool IsAssignPassive; /**< 是否为生成物品分配随机被动；本 mod 固定为 `false`。 */
        float LogDelay;       /**< 游戏通知延迟；本 mod 固定为 0。 */
        bool bNotifyLog;      /**< 是否显示游戏内获得日志；本 mod 固定为 `false`。 */
        int32_t Result;       /**< 游戏函数写回的添加结果枚举数值。 */
    } params{};
    params.StaticItemId = FName(wide.c_str());
    params.Count = count;
    params.IsAssignPassive = false;
    params.LogDelay = 0.0F;
    params.bNotifyLog = false;
    inventory->ProcessEvent(fn, &params);
    Output::send<LogLevel::Warning>(
        STR("give_items: AddItem_ServerInternal('{}', x{}) -> result={}\n"), wide, params.Count,
        params.Result);
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
    if (utility == nullptr || function == nullptr || worldContext == nullptr) {
        return {};
    }
    /** @brief `PalUIUtility:GetItemName` 的反射参数布局。 */
    struct Params {
        UObject* WorldContextObject; /**< 非拥有世界上下文对象。 */
        FName StaticItemId;          /**< 要查询的物品 Raw ID。 */
        FText OutName;               /**< 游戏函数写回的本地化名称。 */
    } params{.WorldContextObject = worldContext, .StaticItemId = id};
    utility->ProcessEvent(function, &params);
    return text_encoding::to_utf8(params.OutName.ToString());
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

    struct Params {
        UObject* WorldContextObject{};
        UObject* ReturnValue{};
    } params{.WorldContextObject = worldContext};
    utility->ProcessEvent(function, &params);
    auto* manager = params.ReturnValue;
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

    const auto layout =
        FScriptMap::GetScriptLayout(keyProperty->GetSize(), keyProperty->GetMinAlignment(),
                                    valueProperty->GetSize(), valueProperty->GetMinAlignment());
    std::vector<FName> discovered;
    discovered.reserve(static_cast<std::size_t>(map->Num()));
    for (int32 index{}; index < map->GetMaxIndex(); ++index) {
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
        if (idProperty == nullptr) {
            return LoopAction::Continue;
        }
        if (FName* id = idProperty->ContainerPtrToValuePtr<FName>(obj)) {
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
