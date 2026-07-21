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

// Read the main bag into a list of {item id, count} (skips empty slots). Game thread only.
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
    UFunction* fnGet =
        UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Pal.PalItemContainer:Get"));
    if (fnGet == nullptr)
    {
        Output::send<LogLevel::Warning>(STR("read_inventory: Get UFunction not found\n"));
        return items;
    }

    int non_empty = 0;
    for (int32_t i = 0; i < num; ++i)
    {
        struct
        {
            int32_t Index;
            UObject* Slot;
        } gp{};
        gp.Index = i;
        container->ProcessEvent(fnGet, &gp);
        UObject* slot = gp.Slot;
        if (slot == nullptr)
        {
            continue;
        }

        // StackCount (int32)
        int32_t count = 0;
        if (FProperty* sc = slot->GetPropertyByNameInChain(STR("StackCount")))
        {
            if (auto* ip = CastField<FIntProperty>(sc))
            {
                count = ip->GetPropertyValueInContainer(slot);
            }
        }

        // ItemId.StaticId (FName) — StaticId is the first field of FPalItemId (offset 0).
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
            items.push_back({name, static_cast<int>(count)});
            ++non_empty;
        }
    }
    Output::send<LogLevel::Warning>(STR("read_inventory: {} slots, {} non-empty items\n"), num, non_empty);
    return items;
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
        ModVersion = STR("0.8.0");
        ModDescription = STR("UE4SS C++ mod for Palworld 1.0");
        ModAuthors = STR("with-fair-wind");

        Output::send<LogLevel::Verbose>(STR("MyPalMod loaded (v0.8.0 inventory list)\n"));

        register_tab(STR("MyPalMod"),
                     [](CppUserModBase* mod)
                     {
                         UE4SS_ENABLE_IMGUI()
                         auto* self = static_cast<MyPalMod*>(mod);
                         ImGui::TextUnformatted("A floating 'MyPalMod' window should be visible ->");
                         if (ImGui::Begin("MyPalMod v0.8", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
                         {
                             // Give items
                             ImGui::TextUnformatted("Give items");
                             ImGui::InputText("Item ID", self->item_buf_, sizeof(self->item_buf_));
                             ImGui::InputInt("Count", &self->count_input_);
                             self->count_input_ = clamp(self->count_input_, 1, 9999);
                             if (ImGui::Button("Give"))
                             {
                                 const std::lock_guard lock(self->give_mutex_);
                                 self->give_item_ = self->item_buf_;
                                 self->give_count_ = self->count_input_;
                                 self->give_requested_ = true;
                             }
                             ImGui::Separator();
                             // Inventory
                             if (ImGui::Button("Refresh inventory"))
                             {
                                 self->want_read_.store(true);
                             }
                             ImGui::SameLine();
                             {
                                 const std::lock_guard lock(self->inv_mutex_);
                                 ImGui::TextDisabled("(%d items)", static_cast<int>(self->inv_cache_.size()));
                             }
                             {
                                 const std::lock_guard lock(self->inv_mutex_);
                                 if (self->inv_cache_.empty())
                                 {
                                     ImGui::TextDisabled("  (empty - click Refresh)");
                                 }
                                 else
                                 {
                                     ImGui::BeginChild("invlist", ImVec2(360, 240), true);
                                     for (const auto& e : self->inv_cache_)
                                     {
                                         ImGui::Text("%-28s x%d", e.item_id.c_str(), e.count);
                                     }
                                     ImGui::EndChild();
                                 }
                             }
                             ImGui::Separator();
                             if (ImGui::Button("Discover"))
                             {
                                 self->want_discover_.store(true);
                             }
                             ImGui::TextWrapped("Item IDs: PalSphere, PalSphere_Tera, Stone, Wood, Money, ... "
                                                "(github.com/KURAMAAA0/PalModding ItemIDs.txt). "
                                                "Results -> UE4SS Console tab.");
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
        // Give (params via mutex)
        std::string item;
        int count = 0;
        bool do_give = false;
        {
            const std::lock_guard lock(give_mutex_);
            if (give_requested_)
            {
                give_requested_ = false;
                item = give_item_;
                count = give_count_;
                do_give = true;
            }
        }
        if (do_give)
        {
            give_items(item, static_cast<int32>(count));
        }
        // Read inventory -> cache (under mutex, for the UI to render)
        if (want_read_.exchange(false))
        {
            auto fresh = read_inventory();
            const std::lock_guard lock(inv_mutex_);
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

    char item_buf_[64] = "PalSphere_Tera";
    int count_input_ = 10;

    std::mutex give_mutex_;
    std::string give_item_;
    int give_count_ = 0;
    bool give_requested_ = false;

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
