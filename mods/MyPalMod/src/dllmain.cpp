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

#include <atomic>
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

        register_tab(STR("MyPalMod"),
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

                                 if (self->selected_ >= 0 &&
                                     self->selected_ < static_cast<int>(self->inv_cache_.size()))
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
    }

private:
    static auto clamp(int v, int lo, int hi) -> int
    {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    // UI state (GUI thread)
    char item_buf_[64] = "PalSphere_Tera";
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
