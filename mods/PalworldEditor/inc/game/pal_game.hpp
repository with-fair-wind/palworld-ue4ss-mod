#pragma once
// pal_game.hpp — Game-interaction functions for PalworldEditor.
// All functions here run on the GAME THREAD (via on_update or hooks).
// They interact with Palworld's UObject system via UE4SS reflection.

#include <algorithm>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <support/text_encoding.hpp>

using namespace RC;
using namespace RC::Unreal;

namespace pal_game {
inline constexpr const TCHAR* kInventoryClassName = STR("PalPlayerInventoryData");

// Basic UObject validity check — catches stale pointers after object destruction.
inline auto is_valid(UObject* obj) -> bool {
    return obj != nullptr && obj->GetClassPrivate() != nullptr;
}

inline constexpr std::wstring_view kDiscoveryKeywords[] = {
    L"Inventory", L"IndividualCharacter", L"ItemContainer", L"Otomo", L"PalCharacterContainer",
};

struct InvEntry {
    std::string item_id;
    int count;
    int32_t slot_index;
};

struct PalEntry {
    std::string name;
    UObject* ptr;
};

// ---------------------------------------------------------------------------
// Inventory access
// ---------------------------------------------------------------------------

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
    struct {
        uint8_t Type;
        UObject* Out;
        bool Ret;
    } p{};
    p.Type = 0;  // EPalPlayerInventoryType::Common
    inv->ProcessEvent(fn, &p);
    return p.Out;
}

inline auto container_num(UObject* container) -> int32_t {
    UFunction* fn = UObjectGlobals::StaticFindObject<UFunction*>(
        nullptr, nullptr, STR("/Script/Pal.PalItemContainer:Num"));
    if (fn == nullptr || container == nullptr) {
        return 0;
    }
    struct {
        int32_t Ret;
    } n{};
    container->ProcessEvent(fn, &n);
    return n.Ret;
}

inline auto container_get(UObject* container, int32_t index) -> UObject* {
    UFunction* fn = UObjectGlobals::StaticFindObject<UFunction*>(
        nullptr, nullptr, STR("/Script/Pal.PalItemContainer:Get"));
    if (fn == nullptr || container == nullptr) {
        return nullptr;
    }
    struct {
        int32_t Index;
        UObject* Slot;
    } gp{};
    gp.Index = index;
    container->ProcessEvent(fn, &gp);
    return gp.Slot;
}

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
    struct {
        FName StaticItemId;
        int32_t Count;
        bool IsAssignPassive;
        float LogDelay;
        bool bNotifyLog;
        int32_t Result;
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

inline auto scan_all_items() -> std::vector<std::string> {
    std::vector<std::string> ids;
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
                ids.emplace_back(text_encoding::to_utf8(w));
            }
        }
        return LoopAction::Continue;
    });
    std::sort(ids.begin(), ids.end());
    Output::send<LogLevel::Warning>(STR("scan_all_items: found {} item definitions\n"),
                                    static_cast<int32>(ids.size()));
    return ids;
}

// ---------------------------------------------------------------------------
// Pal scanning + passive skill editing
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

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
