// MyPalMod — UE4SS C++ mod for Palworld 1.0.
//
//   Build:   cmake --preset ninja-msvc-x64 && cmake --build --preset ninja-msvc-x64
//   Deploy:  cmake --build --preset ninja-msvc-x64 --target deploy
//
// UI is a floating ImGui window inside the UE4SS GUI (select the "MyPalMod" tab -> a
// movable window pops up). NOT the F10 in-game console (broken on Palworld). Button
// clicks stage a request (mutex/atomic); actual game calls run on the game thread in
// on_update (UE reflection isn't thread-safe).
//
//   Give items        - item ID + count -> AddItem_ServerInternal
//   Refresh inventory - read the main bag, list item x count in the window
//   Set count         - select an item, change its StackCount (writes the slot directly)
//   Discover          - scan UObjects, log a histogram of Pal/item classes
//
// Discovered Palworld API (UHTHeaderDump, Pal module):
//   UPalPlayerInventoryData::AddItem_ServerInternal(FName,int32,bool,float,bool)->enum
//   UPalPlayerInventoryData::TryGetContainerFromInventoryType(uint8 type, UPalItemContainer*&)
//   UPalItemContainer::Num() / Get(i) -> UPalItemSlot{ StackCount:int32, ItemId.StaticId:FName }

#include <DynamicOutput/DynamicOutput.hpp>
#include <GUI/GUITab.hpp>
#include <Mod/CppUserModBase.hpp>
#include <UE4SSProgram.hpp>                          // UE4SS_ENABLE_IMGUI
#include <Unreal/CoreUObject/UObject/UnrealType.hpp> // FProperty, FIntProperty, CastField
#include <Unreal/NameTypes.hpp>                      // FName
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <imgui.h>

#include "item_database.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

using namespace RC;
using namespace RC::Unreal;

namespace
{
constexpr const TCHAR* kInventoryClassName = STR("PalPlayerInventoryData");

constexpr std::wstring_view kDiscoveryKeywords[] = {
    L"Inventory",
    L"IndividualCharacter",
    L"ItemContainer",
    L"Otomo",
    L"PalCharacterContainer",
};

struct InvEntry
{
    std::string item_id; // narrow (ASCII) for ImGui
    int count;
    int32_t slot_index;
};

struct PalEntry
{
    std::string name; // CharacterID (species)
    UObject* ptr;     // raw ptr — valid during the session
};

// Get the player's main bag container (EPalPlayerInventoryType::Common == 0), or nullptr.
auto get_main_container() -> UObject*
{
    UObject* inv = UObjectGlobals::FindFirstOf(kInventoryClassName);
    if (inv == nullptr)
    {
        return nullptr;
    }
    UFunction* fn = UObjectGlobals::StaticFindObject<UFunction*>(
        nullptr, nullptr, STR("/Script/Pal.PalPlayerInventoryData:TryGetContainerFromInventoryType"));
    if (fn == nullptr)
    {
        return nullptr;
    }
    struct
    {
        uint8_t Type;
        UObject* Out;
        bool Ret;
    } p{};
    p.Type = 0; // EPalPlayerInventoryType::Common
    inv->ProcessEvent(fn, &p);
    return p.Out;
}

auto container_num(UObject* container) -> int32_t
{
    UFunction* fn =
        UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Pal.PalItemContainer:Num"));
    if (fn == nullptr || container == nullptr)
    {
        return 0;
    }
    struct
    {
        int32_t Ret;
    } n{};
    container->ProcessEvent(fn, &n);
    return n.Ret;
}

// Get slot[i] from a container, or nullptr.
auto container_get(UObject* container, int32_t index) -> UObject*
{
    UFunction* fn =
        UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Pal.PalItemContainer:Get"));
    if (fn == nullptr || container == nullptr)
    {
        return nullptr;
    }
    struct
    {
        int32_t Index;
        UObject* Slot;
    } gp{};
    gp.Index = index;
    container->ProcessEvent(fn, &gp);
    return gp.Slot;
}

auto read_slot_stack_count(UObject* slot) -> int32_t
{
    if (slot == nullptr)
    {
        return 0;
    }
    FProperty* sc = slot->GetPropertyByNameInChain(STR("StackCount"));
    if (auto* ip = CastField<FIntProperty>(sc))
    {
        return ip->GetPropertyValueInContainer(slot);
    }
    return 0;
}

// Read the main bag into a list of {item id, count, slot index}. Game thread only.
auto read_inventory() -> std::vector<InvEntry>
{
    std::vector<InvEntry> items;

    UObject* container = get_main_container();
    if (container == nullptr)
    {
        Output::send<LogLevel::Warning>(STR("read_inventory: container not found (not in-game?)\n"));
        return items;
    }
    const int32_t num = container_num(container);
    int non_empty = 0;
    for (int32_t i = 0; i < num; ++i)
    {
        UObject* slot = container_get(container, i);
        if (slot == nullptr)
        {
            continue;
        }
        const int32_t count = read_slot_stack_count(slot);

        // ItemId.StaticId (FName) — first field of FPalItemId (offset 0).
        std::string name;
        if (FProperty* itemIdProp = slot->GetPropertyByNameInChain(STR("ItemId")))
        {
            if (FName* sid = itemIdProp->ContainerPtrToValuePtr<FName>(slot))
            {
                const std::wstring w = sid->ToString();
                name = std::string(w.begin(), w.end());
            }
        }

        if (count > 0 && !name.empty())
        {
            items.push_back({name, static_cast<int>(count), i});
            ++non_empty;
        }
    }
    Output::send<LogLevel::Warning>(STR("read_inventory: {} slots, {} non-empty items\n"), num, non_empty);
    return items;
}

// Set slot[slot_index].StackCount = new_count (direct write). Game thread only.
auto set_slot_count(int32_t slot_index, int32_t new_count) -> void
{
    UObject* container = get_main_container();
    if (container == nullptr)
    {
        Output::send<LogLevel::Warning>(STR("set_slot_count: container not found\n"));
        return;
    }
    UObject* slot = container_get(container, slot_index);
    if (slot == nullptr)
    {
        Output::send<LogLevel::Warning>(STR("set_slot_count: slot {} not found\n"), slot_index);
        return;
    }
    FProperty* sc = slot->GetPropertyByNameInChain(STR("StackCount"));
    auto* ip = CastField<FIntProperty>(sc);
    if (ip == nullptr)
    {
        Output::send<LogLevel::Warning>(STR("set_slot_count: slot {} has no StackCount property\n"), slot_index);
        return;
    }
    const int32_t old = ip->GetPropertyValueInContainer(slot);
    ip->SetPropertyValueInContainer(slot, new_count);
    Output::send<LogLevel::Warning>(STR("set_slot_count: slot {} StackCount {} -> {}\n"), slot_index, old, new_count);
}

// Scan for PalIndividualCharacterParameter instances (the player's Pals). Game thread only.
auto scan_pals() -> std::vector<PalEntry>
{
    std::vector<PalEntry> pals;
    UObjectGlobals::ForEachUObject(
        [&](UObject* obj, int32_t, int32_t) -> LoopAction
        {
            UClass* cls = obj->GetClassPrivate();
            if (cls == nullptr)
            {
                return LoopAction::Continue;
            }
            if (cls->GetName() != STR("PalIndividualCharacterParameter"))
            {
                return LoopAction::Continue;
            }
            // Read SaveParameter.CharacterID (FName, first field of FPalIndividualCharacterSaveParameter).
            std::string name;
            if (FProperty* spProp = obj->GetPropertyByNameInChain(STR("SaveParameter")))
            {
                if (FName* charId = spProp->ContainerPtrToValuePtr<FName>(obj))
                {
                    const std::wstring w = charId->ToString();
                    name = std::string(w.begin(), w.end());
                }
            }
            if (!name.empty())
            {
                pals.push_back({name, obj});
            }
            return LoopAction::Continue;
        });
    std::sort(pals.begin(), pals.end(), [](const PalEntry& a, const PalEntry& b) { return a.name < b.name; });
    Output::send<LogLevel::Warning>(STR("scan_pals: found {} pals\n"), static_cast<int32>(pals.size()));
    return pals;
}

// Add a passive skill to a Pal. Game thread only.
auto add_passive(UObject* pal, const std::string& skill_id) -> void
{
    if (pal == nullptr)
    {
        return;
    }
    UFunction* fn = UObjectGlobals::StaticFindObject<UFunction*>(
        nullptr, nullptr, STR("/Script/Pal.PalIndividualCharacterParameter:AddPassiveSkill"));
    if (fn == nullptr)
    {
        Output::send<LogLevel::Warning>(STR("add_passive: AddPassiveSkill not found\n"));
        return;
    }
    const std::wstring wide(skill_id.begin(), skill_id.end());
    struct
    {
        FName AddSkill;
        FName OverrideSkill;
    } params{};
    params.AddSkill = FName(wide.c_str());
    // OverrideSkill stays NAME_None = add without replacing
    pal->ProcessEvent(fn, &params);
    Output::send<LogLevel::Warning>(STR("add_passive: AddPassiveSkill('{}')\n"), wide);
}

// Remove a passive skill from a Pal. Game thread only.
auto remove_passive(UObject* pal, const std::string& skill_id) -> void
{
    if (pal == nullptr)
    {
        return;
    }
    UFunction* fn = UObjectGlobals::StaticFindObject<UFunction*>(
        nullptr, nullptr, STR("/Script/Pal.PalIndividualCharacterParameter:RemovePassiveSkill"));
    if (fn == nullptr)
    {
        Output::send<LogLevel::Warning>(STR("remove_passive: RemovePassiveSkill not found\n"));
        return;
    }
    const std::wstring wide(skill_id.begin(), skill_id.end());
    struct
    {
        FName SkillId;
    } params{};
    params.SkillId = FName(wide.c_str());
    pal->ProcessEvent(fn, &params);
    Output::send<LogLevel::Warning>(STR("remove_passive: RemovePassiveSkill('{}')\n"), wide);
}

// Read a Pal's current passive skills via GetPassiveSkillList() and log them.
// Shows the real FName format so the user knows what to type for Add/Remove. Game thread only.
auto read_pal_passives(UObject* pal) -> void
{
    if (pal == nullptr)
    {
        return;
    }
    UFunction* fn = UObjectGlobals::StaticFindObject<UFunction*>(
        nullptr, nullptr, STR("/Script/Pal.PalIndividualCharacterParameter:GetPassiveSkillList"));
    if (fn == nullptr)
    {
        Output::send<LogLevel::Warning>(STR("read_pal_passives: GetPassiveSkillList not found\n"));
        return;
    }
    // TArray<FName> return layout: {FName* Data; int32 Num; int32 Max}
    struct
    {
        FName* Data;
        int32_t Num;
        int32_t Max;
    } ret{};
    pal->ProcessEvent(fn, &ret);
    Output::send<LogLevel::Warning>(STR("read_pal_passives: {} passive(s):\n"), ret.Num);
    for (int32_t i = 0; i < ret.Num && i < 10 && ret.Data != nullptr; ++i)
    {
        const std::wstring w = ret.Data[i].ToString();
        Output::send<LogLevel::Warning>(STR("  [{}] {}\n"), i, w);
    }
}

// Scan all UObjects for UPalStaticItemDataBase instances -> collect every item ID that
// exists in the current game version. Replaces the hardcoded item_database.h list.
// One-time full scan (~1s for 270k objects). Game thread only.
auto scan_all_items() -> std::vector<std::string>
{
    std::vector<std::string> ids;
    UObjectGlobals::ForEachUObject(
        [&](UObject* obj, int32_t, int32_t) -> LoopAction
        {
            UClass* cls = obj->GetClassPrivate();
            if (cls == nullptr)
            {
                return LoopAction::Continue;
            }
            const std::wstring name = cls->GetName();
            // Must start with "PalStaticItemData" but exclude container/manager types.
            if (name.find(L"PalStaticItemData") != 0)
            {
                return LoopAction::Continue;
            }
            if (name.find(L"Table") != std::wstring::npos || name.find(L"Asset") != std::wstring::npos ||
                name.find(L"Manager") != std::wstring::npos || name.find(L"Struct") != std::wstring::npos ||
                name.find(L"AndNum") != std::wstring::npos || name.find(L"RowName") != std::wstring::npos)
            {
                return LoopAction::Continue;
            }
            // Must have an ID property (confirms it's an item-data instance, not a helper).
            FProperty* idProp = obj->GetPropertyByNameInChain(STR("ID"));
            if (idProp == nullptr)
            {
                return LoopAction::Continue;
            }
            if (FName* id = idProp->ContainerPtrToValuePtr<FName>(obj))
            {
                const std::wstring w = id->ToString();
                if (!w.empty())
                {
                    ids.emplace_back(w.begin(), w.end());
                }
            }
            return LoopAction::Continue;
        });
    std::sort(ids.begin(), ids.end());
    Output::send<LogLevel::Warning>(STR("scan_all_items: found {} item definitions\n"), static_cast<int32>(ids.size()));
    return ids;
}

// Add `count` of `item_id` to the player's inventory. Game thread only.
auto give_items(const std::string& item_id, int32 count) -> void
{
    UObject* inventory = UObjectGlobals::FindFirstOf(kInventoryClassName);
    if (inventory == nullptr)
    {
        Output::send<LogLevel::Warning>(STR("give_items: inventory not found (not in-game?)\n"));
        return;
    }
    UFunction* fn = UObjectGlobals::StaticFindObject<UFunction*>(
        nullptr, nullptr, STR("/Script/Pal.PalPlayerInventoryData:AddItem_ServerInternal"));
    if (fn == nullptr)
    {
        Output::send<LogLevel::Warning>(STR("give_items: AddItem_ServerInternal not found\n"));
        return;
    }
    const std::wstring wide(item_id.begin(), item_id.end());
    struct
    {
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
        STR("give_items: AddItem_ServerInternal('{}', x{}) -> result={}\n"), wide, params.Count, params.Result);
}

auto discover_objects() -> void
{
    Output::send<LogLevel::Warning>(STR("=== MyPalMod discovery: scanning UObjects ===\n"));
    std::map<std::wstring, int> matching;
    int total = 0;
    UObjectGlobals::ForEachUObject(
        [&](UObject* obj, int32_t, int32_t) -> LoopAction
        {
            ++total;
            UClass* cls = obj->GetClassPrivate();
            if (cls == nullptr)
            {
                return LoopAction::Continue;
            }
            std::wstring name = cls->GetName();
            for (auto kw : kDiscoveryKeywords)
            {
                if (name.find(kw) != std::wstring::npos)
                {
                    ++matching[name];
                    break;
                }
            }
            return LoopAction::Continue;
        });
    Output::send<LogLevel::Warning>(
        STR("=== discovery: {} total objects, {} matching class types ===\n"), total, matching.size());
    int n = 0;
    for (const auto& [cls_name, cnt] : matching)
    {
        Output::send<LogLevel::Warning>(STR("[discover] {} (x{})\n"), cls_name, cnt);
        if (++n >= 200)
        {
            break;
        }
    }
    Output::send<LogLevel::Warning>(STR("=== discovery done ===\n"));
}
} // namespace

class MyPalMod final : public CppUserModBase
{
public:
    MyPalMod() : CppUserModBase()
    {
        ModName = STR("MyPalMod");
        ModVersion = STR("0.9.0");
        ModDescription = STR("UE4SS C++ mod for Palworld 1.0");
        ModAuthors = STR("with-fair-wind");

        Output::send<LogLevel::Verbose>(STR("MyPalMod loaded (v0.9.0 modify)\n"));

        register_tab(
            STR("MyPalMod"),
            [](CppUserModBase* mod)
            {
                UE4SS_ENABLE_IMGUI()
                auto* self = static_cast<MyPalMod*>(mod);
                ImGui::TextUnformatted("A floating 'MyPalMod' window should be visible ->");
                if (ImGui::Begin("MyPalMod v0.9", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
                {
                    // --- Give items ---
                    ImGui::TextUnformatted("Give items");
                    ImGui::InputText("Item ID", self->item_buf_, sizeof(self->item_buf_));
                    ImGui::InputInt("Count", &self->count_input_);
                    self->count_input_ = clamp(self->count_input_, 1, 9999);
                    if (ImGui::Button("Give"))
                    {
                        const std::lock_guard lock(self->req_mutex_);
                        self->give_item_ = self->item_buf_;
                        self->give_count_ = self->count_input_;
                        self->give_requested_ = true;
                    }
                    ImGui::Separator();

                    // --- Item browser (dynamic scan or curated fallback) ---
                    if (ImGui::Button("Scan game items"))
                    {
                        self->want_scan_items_.store(true);
                    }
                    ImGui::SameLine();
                    ImGui::InputText("##search", self->search_buf_, sizeof(self->search_buf_));
                    {
                        const std::lock_guard lock(self->inv_mutex_);
                        ImGui::TextDisabled("(%d items)",
                                            self->item_db_cache_.empty()
                                                ? kBrowseItemCount
                                                : static_cast<int>(self->item_db_cache_.size()));
                    }
                    ImGui::BeginChild("browser", ImVec2(380, 160), true);
                    {
                        std::string filter(self->search_buf_);
                        for (auto& c : filter)
                        {
                            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        }
                        auto tryItem = [&](const char* raw)
                        {
                            std::string lower(raw);
                            for (auto& c : lower)
                            {
                                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                            }
                            if (!filter.empty() && lower.find(filter) == std::string::npos)
                            {
                                return;
                            }
                            if (ImGui::Selectable(raw))
                            {
                                std::strncpy(self->item_buf_, raw, sizeof(self->item_buf_) - 1);
                                self->item_buf_[sizeof(self->item_buf_) - 1] = '\0';
                            }
                        };
                        const std::lock_guard lock(self->inv_mutex_);
                        if (!self->item_db_cache_.empty())
                        {
                            for (const auto& item : self->item_db_cache_)
                            {
                                tryItem(item.c_str());
                            }
                        }
                        else
                        {
                            for (int bi = 0; bi < kBrowseItemCount; ++bi)
                            {
                                tryItem(kBrowseItems[bi]);
                            }
                        }
                    }
                    ImGui::EndChild();

                    // --- Inventory (list + modify) ---
                    if (ImGui::Button("Refresh inventory"))
                    {
                        self->want_read_.store(true);
                    }
                    ImGui::SameLine();
                    ImGui::TextUnformatted("(click an item to select, then set count)");
                    {
                        const std::lock_guard lock(self->inv_mutex_);
                        ImGui::BeginChild("invlist", ImVec2(380, 220), true);
                        for (int i = 0; i < static_cast<int>(self->inv_cache_.size()); ++i)
                        {
                            const auto& e = self->inv_cache_[i];
                            const std::string label = e.item_id + "  x" + std::to_string(e.count);
                            if (ImGui::Selectable(label.c_str(), self->selected_ == i))
                            {
                                self->selected_ = i;
                                self->set_count_input_ = e.count;
                            }
                        }
                        ImGui::EndChild();

                        if (self->selected_ >= 0 && self->selected_ < static_cast<int>(self->inv_cache_.size()))
                        {
                            const auto& e = self->inv_cache_[self->selected_];
                            ImGui::Text("Selected: %s (slot %d, x%d)",
                                        e.item_id.c_str(),
                                        static_cast<int>(e.slot_index),
                                        e.count);
                            ImGui::InputInt("New count", &self->set_count_input_);
                            self->set_count_input_ = clamp(self->set_count_input_, 0, 9999);
                            if (ImGui::Button("Set count"))
                            {
                                const std::lock_guard lock(self->req_mutex_);
                                self->modify_slot_ = e.slot_index;
                                self->modify_count_ = self->set_count_input_;
                                self->modify_requested_ = true;
                            }
                        }
                    }
                    ImGui::Separator();

                    // --- Pal editor ---
                    if (ImGui::CollapsingHeader("Pal editor"))
                    {
                        if (ImGui::Button("Scan Pals"))
                        {
                            self->want_scan_pals_.store(true);
                        }
                        ImGui::SameLine();
                        {
                            const std::lock_guard lock(self->inv_mutex_);
                            ImGui::TextDisabled("(%d pals)", static_cast<int>(self->pal_cache_.size()));
                        }
                        ImGui::BeginChild("pallist", ImVec2(380, 140), true);
                        {
                            const std::lock_guard lock(self->inv_mutex_);
                            for (int i = 0; i < static_cast<int>(self->pal_cache_.size()); ++i)
                            {
                                if (ImGui::Selectable(self->pal_cache_[i].name.c_str(), self->pal_selected_ == i))
                                {
                                    self->pal_selected_ = i;
                                }
                            }
                        }
                        ImGui::EndChild();
                        UObject* selPal = nullptr;
                        {
                            const std::lock_guard lock(self->inv_mutex_);
                            if (self->pal_selected_ >= 0 &&
                                self->pal_selected_ < static_cast<int>(self->pal_cache_.size()))
                            {
                                selPal = self->pal_cache_[self->pal_selected_].ptr;
                            }
                        }
                        if (selPal != nullptr)
                        {
                            ImGui::InputText("Passive SkillId", self->passive_buf_, sizeof(self->passive_buf_));
                            if (ImGui::Button("Add Passive"))
                            {
                                const std::lock_guard lock(self->req_mutex_);
                                self->passive_pal_ = selPal;
                                self->passive_skill_ = self->passive_buf_;
                                self->passive_add_ = true;
                                self->passive_requested_ = true;
                            }
                            ImGui::SameLine();
                            if (ImGui::Button("Remove Passive"))
                            {
                                const std::lock_guard lock(self->req_mutex_);
                                self->passive_pal_ = selPal;
                                self->passive_skill_ = self->passive_buf_;
                                self->passive_add_ = false;
                                self->passive_requested_ = true;
                            }
                            ImGui::SameLine();
                            if (ImGui::Button("Read Passives"))
                            {
                                const std::lock_guard lock(self->req_mutex_);
                                self->passive_pal_ = selPal;
                                self->passive_read_requested_ = true;
                            }
                        }
                    }

                    ImGui::Separator();
                    if (ImGui::Button("Discover"))
                    {
                        self->want_discover_.store(true);
                    }
                }
                ImGui::End();
            });
    }

    ~MyPalMod() override = default;

    auto on_unreal_init() -> void override
    {
        if (const auto object =
                UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/CoreUObject.Object")))
        {
            Output::send<LogLevel::Verbose>(STR("Object Name: {}\n"), object->GetFullName());
        }
    }

    auto on_update() -> void override
    {
        // Pull requests (give / modify) under the mutex, then execute outside it.
        std::string item;
        int count = 0;
        bool do_give = false;
        int32_t mod_slot = 0;
        int32_t mod_count = 0;
        bool do_mod = false;
        {
            const std::lock_guard lock(req_mutex_);
            if (give_requested_)
            {
                give_requested_ = false;
                item = give_item_;
                count = give_count_;
                do_give = true;
            }
            if (modify_requested_)
            {
                modify_requested_ = false;
                mod_slot = modify_slot_;
                mod_count = modify_count_;
                do_mod = true;
            }
        }
        if (do_give)
        {
            give_items(item, static_cast<int32>(count));
            want_read_.store(true); // refresh the list after giving
        }
        if (do_mod)
        {
            set_slot_count(mod_slot, mod_count);
            want_read_.store(true); // refresh the list after modifying
        }
        if (want_read_.exchange(false))
        {
            auto fresh = read_inventory();
            const std::lock_guard lock(inv_mutex_);
            // Invalidate selection if it's now out of range.
            if (selected_ >= static_cast<int>(fresh.size()))
            {
                selected_ = -1;
            }
            inv_cache_ = std::move(fresh);
        }
        if (want_discover_.exchange(false))
        {
            discover_objects();
        }
        if (want_scan_items_.exchange(false))
        {
            auto fresh = scan_all_items();
            const std::lock_guard lock(inv_mutex_);
            item_db_cache_ = std::move(fresh);
        }
        if (want_scan_pals_.exchange(false))
        {
            auto fresh = scan_pals();
            const std::lock_guard lock(inv_mutex_);
            if (pal_selected_ >= static_cast<int>(fresh.size()))
            {
                pal_selected_ = -1;
            }
            pal_cache_ = std::move(fresh);
        }
        if (passive_requested_)
        {
            UObject* pal = nullptr;
            std::string skill;
            bool doAdd = false;
            {
                const std::lock_guard lock(req_mutex_);
                if (passive_requested_)
                {
                    passive_requested_ = false;
                    pal = passive_pal_;
                    skill = passive_skill_;
                    doAdd = passive_add_;
                }
            }
            if (pal != nullptr && !skill.empty())
            {
                if (doAdd)
                {
                    add_passive(pal, skill);
                }
                else
                {
                    remove_passive(pal, skill);
                }
            }
        }
        if (passive_read_requested_)
        {
            UObject* readPal = nullptr;
            {
                const std::lock_guard lock(req_mutex_);
                if (passive_read_requested_)
                {
                    passive_read_requested_ = false;
                    readPal = passive_pal_;
                }
            }
            if (readPal != nullptr)
            {
                read_pal_passives(readPal);
            }
        }
    }

private:
    static auto clamp(int v, int lo, int hi) -> int
    {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    // UI state (GUI thread)
    char item_buf_[64] = "PalSphere_Tera";
    char search_buf_[64]{};
    int count_input_ = 10;
    int set_count_input_ = 0;
    int selected_ = -1;

    // Request handoff (GUI -> game thread)
    std::mutex req_mutex_;
    std::string give_item_;
    int give_count_ = 0;
    bool give_requested_ = false;
    int32_t modify_slot_ = 0;
    int32_t modify_count_ = 0;
    bool modify_requested_ = false;

    // Inventory cache (game thread writes, GUI thread reads)
    std::mutex inv_mutex_;
    std::vector<InvEntry> inv_cache_;

    std::atomic<bool> want_read_{false};
    std::atomic<bool> want_discover_{false};
    std::atomic<bool> want_scan_items_{false};
    std::vector<std::string> item_db_cache_; // populated by scan_all_items, under inv_mutex_

    // Pal editor
    char passive_buf_[64]{};
    int pal_selected_ = -1;
    std::vector<PalEntry> pal_cache_; // under inv_mutex_
    std::atomic<bool> want_scan_pals_{false};
    UObject* passive_pal_{nullptr};
    std::string passive_skill_;
    bool passive_add_{false};
    bool passive_requested_{false};
    bool passive_read_requested_{false};
};

#define MYPALMOD_API __declspec(dllexport)
extern "C"
{
    MYPALMOD_API CppUserModBase* start_mod()
    {
        return new MyPalMod();
    }

    MYPALMOD_API void uninstall_mod(CppUserModBase* mod)
    {
        delete mod;
    }
}
