/**
 * @file pal_game.hpp
 * @brief 提供 PalworldEditor 对背包、物品、帕鲁对象和诊断扫描的游戏反射适配接口。
 * @details 本文件中的函数通过 UE4SS 访问 Unreal UObject 和 `ProcessEvent`。除纯常量外，
 *          所有接口都必须在 Unreal 初始化完成后的游戏线程调用；返回的 Unreal 裸指针均为
 *          非拥有观察指针，不会延长游戏对象生命周期。
 */
#pragma once

#include <algorithm>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/FText.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <items/item_catalog.hpp>
#include <support/text_encoding.hpp>

using namespace RC;
using namespace RC::Unreal;

/** @brief 封装 PalworldEditor 直接调用的游戏线程反射操作。 */
namespace pal_game {
/** @brief 主玩家背包数据对象的 Unreal 类名。 */
inline constexpr const TCHAR* kInventoryClassName = STR("PalPlayerInventoryData");

/**
 * @brief 对 UObject 观察指针执行轻量有效性检查。
 * @param[in] obj 待检查的非拥有 UObject 指针。
 * @retval true 指针非空且仍能取得类元数据。
 * @retval false 指针为空或对象的类元数据已经失效。
 * @warning 本检查不能延长对象生命周期，也不能保证对象在后续帧仍然有效。
 */
inline auto is_valid(UObject* obj) -> bool {
    return obj != nullptr && obj->GetClassPrivate() != nullptr;
}

/**
 * @brief 获取可安全作为 Pal 蓝图工具函数世界上下文的玩家背包对象。
 * @return 找到时返回 `PalPlayerInventoryData` 的非拥有观察指针。
 * @retval nullptr 玩家背包对象尚未加载或已经无效。
 * @warning 只能在游戏线程调用，返回值不能跨帧缓存。
 */
[[nodiscard]] inline auto get_world_context() -> UObject* {
    auto* const worldContext = UObjectGlobals::FindFirstOf(kInventoryClassName);
    return is_valid(worldContext) ? worldContext : nullptr;
}

/**
 * @brief 当前待出战帕鲁的运行时解析结果。
 */
struct SelectedPalTarget {
    /** @brief 当前帕鲁的个体参数对象；解析失败时为空。 */
    UObject* parameter{};

    /** @brief 帕鲁 `CharacterID` 的 UTF-8 表示。 */
    std::string characterId;
};

/**
 * @brief 解析 Q/E 当前选中的下一只待出战帕鲁。
 * @return 参数对象与 `CharacterID`；任一步骤失败时返回空结果。
 * @details 从稳定的 `PalPlayerInventoryData` 世界上下文开始，依次取得 Otomo holder、
 *          当前选中槽位、个体 handle 和个体 parameter，不缓存任何中间裸指针。
 * @warning 只能在游戏线程调用；返回的参数对象只允许在当前帧使用。
 */
[[nodiscard]] inline auto resolve_selected_otomo() -> SelectedPalTarget {
    auto* const worldContext = get_world_context();
    auto* const utility = UObjectGlobals::StaticFindObject<UObject*>(
        nullptr, nullptr, STR("/Script/Pal.Default__PalUtility"));
    auto* const getHolderFunction = UObjectGlobals::StaticFindObject<UFunction*>(
        nullptr, nullptr, STR("/Script/Pal.PalUtility:GetOtomoHolderComponent"));
    if (worldContext == nullptr || utility == nullptr || getHolderFunction == nullptr) {
        return {};
    }

    /** @brief `PalUtility:GetOtomoHolderComponent` 的反射参数布局。 */
    struct GetHolderParams {
        UObject* WorldContextObject{}; /**< 非拥有世界上下文对象。 */
        UObject* ReturnValue{};        /**< 游戏写回的非拥有 holder 对象。 */
    } getHolderParams{.WorldContextObject = worldContext};
    utility->ProcessEvent(getHolderFunction, &getHolderParams);
    auto* const holder = getHolderParams.ReturnValue;
    if (!is_valid(holder)) {
        return {};
    }

    auto* const getSelectedFunction = UObjectGlobals::StaticFindObject<UFunction*>(
        nullptr, nullptr,
        STR("/Script/Pal.PalOtomoHolderComponentBase:GetSelectedOtomoID"));
    if (getSelectedFunction == nullptr) {
        return {};
    }
    /** @brief `PalOtomoHolderComponentBase:GetSelectedOtomoID` 的返回参数布局。 */
    struct GetSelectedParams {
        int32_t ReturnValue{-1}; /**< 游戏写回的当前选中 Otomo 槽位索引。 */
    } getSelectedParams;
    holder->ProcessEvent(getSelectedFunction, &getSelectedParams);
    if (getSelectedParams.ReturnValue < 0) {
        return {};
    }

    auto* const getHandleFunction = UObjectGlobals::StaticFindObject<UFunction*>(
        nullptr, nullptr,
        STR("/Script/Pal.PalOtomoHolderComponentBase:GetOtomoIndividualHandle"));
    if (getHandleFunction == nullptr) {
        return {};
    }
    /** @brief `PalOtomoHolderComponentBase:GetOtomoIndividualHandle` 的反射参数布局。 */
    struct GetHandleParams {
        int32_t SlotIndex{};     /**< 要解析的当前选中槽位。 */
        UObject* ReturnValue{}; /**< 游戏写回的非拥有个体 handle。 */
    } getHandleParams{.SlotIndex = getSelectedParams.ReturnValue};
    holder->ProcessEvent(getHandleFunction, &getHandleParams);
    auto* const handle = getHandleParams.ReturnValue;
    if (!is_valid(handle)) {
        return {};
    }

    auto* const getParameterFunction = UObjectGlobals::StaticFindObject<UFunction*>(
        nullptr, nullptr,
        STR("/Script/Pal.PalIndividualCharacterHandle:TryGetIndividualParameter"));
    if (getParameterFunction == nullptr) {
        return {};
    }
    /** @brief `PalIndividualCharacterHandle:TryGetIndividualParameter` 的返回参数布局。 */
    struct GetParameterParams {
        UObject* ReturnValue{}; /**< 游戏写回的非拥有个体参数对象。 */
    } getParameterParams;
    handle->ProcessEvent(getParameterFunction, &getParameterParams);
    auto* const parameter = getParameterParams.ReturnValue;
    if (!is_valid(parameter)) {
        return {};
    }

    auto* const expectedClass = UObjectGlobals::StaticFindObject<UClass*>(
        nullptr, nullptr, STR("/Script/Pal.PalIndividualCharacterParameter"));
    if (expectedClass == nullptr || !parameter->GetClassPrivate()->IsChildOf(expectedClass)) {
        return {};
    }

    auto* const getCharacterIdFunction = UObjectGlobals::StaticFindObject<UFunction*>(
        nullptr, nullptr,
        STR("/Script/Pal.PalIndividualCharacterParameter:GetCharacterID"));
    if (getCharacterIdFunction == nullptr) {
        return {};
    }
    /** @brief `PalIndividualCharacterParameter:GetCharacterID` 的返回参数布局。 */
    struct GetCharacterIdParams {
        FName ReturnValue; /**< 游戏写回的帕鲁 CharacterID。 */
    } getCharacterIdParams;
    parameter->ProcessEvent(getCharacterIdFunction, &getCharacterIdParams);

    return {
        .parameter = parameter,
        .characterId = text_encoding::to_utf8(getCharacterIdParams.ReturnValue.ToString()),
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
 * @brief 表示当前已加载的一只帕鲁及其非拥有对象指针。
 * @warning `ptr` 依赖游戏对象生命周期，跨帧使用前必须重新调用 is_valid()。
 */
struct PalEntry {
    std::string name; /**< Raw 物种名及 `[boxed]`/`[active]` 状态后缀。 */
    UObject* ptr;     /**< 指向 `PalIndividualCharacterParameter` 的非拥有观察指针。 */
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
 * @brief 扫描当前已加载的物品定义并建立可搜索的本地化目录。
 * @return 已去重、排序并建立 Raw ID 标签索引的物品目录快照。
 * @details 扫描名称以 `PalStaticItemData` 开头的 UObject，过滤 Table、Asset、Manager、
 *          Struct、AndNum 和 RowName 辅助类型，从 `ID` 属性读取 Raw ID，并通过
 *          `PalUIUtility:GetItemName` 查询当前游戏语言名称。
 * @note 扫描范围只包含调用时已经加载的 UObject；名称解析失败时目录标签回退到 Raw ID。
 * @warning 只能在游戏线程调用。
 */
inline auto scan_all_items() -> item_catalog::ItemCatalogSnapshot {
    std::vector<item_catalog::ItemOption> items;
    auto* utility = get_ui_utility();
    auto* function = UObjectGlobals::StaticFindObject<UFunction*>(
        nullptr, nullptr, STR("/Script/Pal.PalUIUtility:GetItemName"));
    auto* worldContext = UObjectGlobals::FindFirstOf(kInventoryClassName);
    UObjectGlobals::ForEachUObject([&](UObject* obj, int32_t, int32_t) -> LoopAction {
        UClass* cls = obj->GetClassPrivate();
        if (cls == nullptr) {
            return LoopAction::Continue;
        }
        const std::wstring name = cls->GetName();
        if (name.find(L"PalStaticItemData") != 0) {
            return LoopAction::Continue;
        }
        if (name.find(L"Table") != std::wstring::npos ||
            name.find(L"Asset") != std::wstring::npos ||
            name.find(L"Manager") != std::wstring::npos ||
            name.find(L"Struct") != std::wstring::npos ||
            name.find(L"AndNum") != std::wstring::npos ||
            name.find(L"RowName") != std::wstring::npos) {
            return LoopAction::Continue;
        }
        FProperty* idProp = obj->GetPropertyByNameInChain(STR("ID"));
        if (idProp == nullptr) {
            return LoopAction::Continue;
        }
        if (FName* id = idProp->ContainerPtrToValuePtr<FName>(obj)) {
            const std::wstring w = id->ToString();
            if (!w.empty()) {
                items.push_back(
                    {.id = text_encoding::to_utf8(w),
                     .localizedName = localized_item_name(utility, function, worldContext, *id)});
            }
        }
        return LoopAction::Continue;
    });
    auto catalog = item_catalog::make_item_catalog(std::move(items));
    Output::send<LogLevel::Warning>(STR("scan_all_items: found {} item definitions\n"),
                                    static_cast<int32>(catalog.items.size()));
    return catalog;
}

/**
 * @brief 扫描当前已加载的帕鲁个体参数对象。
 * @return 按展示名称排序的帕鲁快照列表。
 * @details 只收集类名为 `PalIndividualCharacterParameter` 的对象，从 `SaveParameter`
 *          读取 Raw 物种名，并根据 `IndividualActor` 是否存在追加 `[active]` 或 `[boxed]`。
 * @warning 返回的 `PalEntry::ptr` 是非拥有观察指针，跨帧使用前必须重新校验。
 * @warning 只能在游戏线程调用。
 */
inline auto scan_pals() -> std::vector<PalEntry> {
    std::vector<PalEntry> pals;
    UObjectGlobals::ForEachUObject([&](UObject* obj, int32_t, int32_t) -> LoopAction {
        UClass* cls = obj->GetClassPrivate();
        if (cls == nullptr) {
            return LoopAction::Continue;
        }
        if (cls->GetName() != STR("PalIndividualCharacterParameter")) {
            return LoopAction::Continue;
        }
        std::string name;
        if (FProperty* spProp = obj->GetPropertyByNameInChain(STR("SaveParameter"))) {
            if (FName* charId = spProp->ContainerPtrToValuePtr<FName>(obj)) {
                const std::wstring w = charId->ToString();
                name = text_encoding::to_utf8(w);
            }
        }
        if (!name.empty()) {
            bool isBoxed = true;
            if (FProperty* iaProp = obj->GetPropertyByNameInChain(STR("IndividualActor"))) {
                if (void** actorPtr = iaProp->ContainerPtrToValuePtr<void*>(obj)) {
                    isBoxed = (*actorPtr == nullptr);
                }
            }
            name += isBoxed ? " [boxed]" : " [active]";
            pals.push_back({name, obj});
        }
        return LoopAction::Continue;
    });
    std::sort(pals.begin(), pals.end(),
              [](const PalEntry& a, const PalEntry& b) { return a.name < b.name; });
    Output::send<LogLevel::Warning>(STR("scan_pals: found {} pals\n"),
                                    static_cast<int32>(pals.size()));
    return pals;
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
