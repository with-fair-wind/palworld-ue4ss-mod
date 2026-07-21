// MyPalMod — UE4SS C++ mod for Palworld 1.0.
//
//   Build:   cmake --preset ninja-msvc-x64 && cmake --build --preset ninja-msvc-x64
//   Deploy:  cmake --build --preset ninja-msvc-x64 --target deploy
//
// UI is a floating ImGui window inside the UE4SS GUI (select the "MyPalMod" tab and a
// movable window pops up). NOT the F10 in-game console (broken by Palworld's ambiguous
// ConsoleManager signature). Button clicks stage a request (mutex/atomic); the actual
// game-function calls run on the game thread in on_update (UE reflection isn't thread-safe).
//
//   Give items       - type an item ID + count, click Give -> AddItem_ServerInternal
//   Discover         - scan UObjects, log a histogram of Pal/item classes
//   Probe inventory  - verify we can reach the player's item container (logs container + slot count)
//
// Discovered Palworld API (from UHTHeaderDump, Pal module):
//   Items : UPalPlayerInventoryData::AddItem_ServerInternal(FName,int32,bool,float,bool)->EPalItemOperationResult
//           UPalPlayerInventoryData::TryGetContainerFromInventoryType(EPalPlayerInventoryType, UPalItemContainer*&)
//           UPalItemContainer::Num() / Get(i) -> UPalItemSlot
//           UPalItemSlot: StackCount (int32), ItemId.StaticId (FName)

#include <DynamicOutput/DynamicOutput.hpp>
#include <GUI/GUITab.hpp>
#include <Mod/CppUserModBase.hpp>
#include <UE4SSProgram.hpp>                          // UE4SS_ENABLE_IMGUI
#include <Unreal/CoreUObject/UObject/UnrealType.hpp> // FProperty, CastField
#include <Unreal/NameTypes.hpp>                      // FName
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <imgui.h>

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

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

// Add `count` of `item_id` to the player's inventory via AddItem_ServerInternal. Game thread only.
auto give_items(const std::string& item_id, int32 count) -> void
{
    Output::send<LogLevel::Warning>(STR("=== MyPalMod give_items: start ===\n"));

    UObject* inventory = UObjectGlobals::FindFirstOf(kInventoryClassName);
    if (inventory == nullptr)
    {
        Output::send<LogLevel::Warning>(STR("give_items: '{}' instance not found (not in-game?)\n"),
                                        kInventoryClassName);
        return;
    }

    UFunction* fn = UObjectGlobals::StaticFindObject<UFunction*>(
        nullptr, nullptr, STR("/Script/Pal.PalPlayerInventoryData:AddItem_ServerInternal"));
    if (fn == nullptr)
    {
        Output::send<LogLevel::Warning>(STR("give_items: AddItem_ServerInternal UFunction not found\n"));
        return;
    }

    const std::wstring wide(item_id.begin(), item_id.end());

    struct FAddItemServerInternalParams
    {
        FName StaticItemId;
        int32_t Count;
        bool IsAssignPassive;
        float LogDelay;
        bool bNotifyLog;
        int32_t Result;
    };
    FAddItemServerInternalParams params{};
    params.StaticItemId = FName(wide.c_str());
    params.Count = count;
    params.IsAssignPassive = false;
    params.LogDelay = 0.0F;
    params.bNotifyLog = false;

    inventory->ProcessEvent(fn, &params);
    Output::send<LogLevel::Warning>(
        STR("give_items: AddItem_ServerInternal('{}', x{}) -> result={}\n"), wide, params.Count, params.Result);
}

// Verify we can reach the player's main inventory container and read its slot count.
// Step 1 of the inventory-editor feature. Game thread only.
auto probe_inventory() -> void
{
    Output::send<LogLevel::Warning>(STR("=== MyPalMod probe_inventory ===\n"));

    UObject* inventory = UObjectGlobals::FindFirstOf(kInventoryClassName);
    if (inventory == nullptr)
    {
        Output::send<LogLevel::Warning>(STR("probe: inventory not found (not in-game?)\n"));
        return;
    }

    // bool TryGetContainerFromInventoryType(EPalPlayerInventoryType inventoryType, UPalItemContainer*& OutContainer)
    // EPalPlayerInventoryType is uint8; Common == 0 (the main bag). Layout: {uint8@0, ptr@8, bool ret@16}.
    UFunction* fnGetContainer = UObjectGlobals::StaticFindObject<UFunction*>(
        nullptr, nullptr, STR("/Script/Pal.PalPlayerInventoryData:TryGetContainerFromInventoryType"));
    if (fnGetContainer == nullptr)
    {
        Output::send<LogLevel::Warning>(STR("probe: TryGetContainerFromInventoryType UFunction not found\n"));
        return;
    }
    struct FGetContainerParams
    {
        uint8_t InventoryType;
        UObject* OutContainer;
        bool Result;
    };
    FGetContainerParams gp{};
    gp.InventoryType = 0; // EPalPlayerInventoryType::Common
    gp.OutContainer = nullptr;
    inventory->ProcessEvent(fnGetContainer, &gp);

    if (gp.OutContainer == nullptr)
    {
        Output::send<LogLevel::Warning>(STR("probe: container is null (ret={})\n"), gp.Result ? 1 : 0);
        return;
    }
    Output::send<LogLevel::Warning>(STR("probe: container = {}\n"), gp.OutContainer->GetFullName());

    // int32 UPalItemContainer::Num()
    UFunction* fnNum =
        UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Pal.PalItemContainer:Num"));
    if (fnNum == nullptr)
    {
        Output::send<LogLevel::Warning>(STR("probe: Num UFunction not found\n"));
        return;
    }
    struct FNumParams
    {
        int32_t Result;
    };
    FNumParams np{};
    gp.OutContainer->ProcessEvent(fnNum, &np);
    Output::send<LogLevel::Warning>(STR("probe: slot count (Num) = {}\n"), np.Result);
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
        ModVersion = STR("0.7.0");
        ModDescription = STR("UE4SS C++ mod for Palworld 1.0");
        ModAuthors = STR("with-fair-wind");

        Output::send<LogLevel::Verbose>(STR("MyPalMod loaded (v0.7.0 floating window)\n"));

        // Floating ImGui window inside the UE4SS GUI. Select the "MyPalMod" tab and the
        // window pops up (movable). Render runs on the GUI thread -> only stages requests.
        register_tab(STR("MyPalMod"),
                     [](CppUserModBase* mod)
                     {
                         UE4SS_ENABLE_IMGUI()
                         auto* self = static_cast<MyPalMod*>(mod);
                         ImGui::TextUnformatted("A floating 'MyPalMod' window should be visible ->");
                         if (ImGui::Begin("MyPalMod v0.7", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
                         {
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
                             if (ImGui::Button("Probe inventory"))
                             {
                                 self->want_probe_.store(true);
                             }
                             ImGui::SameLine();
                             if (ImGui::Button("Discover"))
                             {
                                 self->want_discover_.store(true);
                             }
                             ImGui::Separator();
                             ImGui::TextWrapped("Item IDs (bare): PalSphere, PalSphere_Tera, Stone, Wood, "
                                                "Money, ... Full list: github.com/KURAMAAA0/PalModding "
                                                "ItemIDs.txt. Results -> UE4SS Console tab.");
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
        if (want_probe_.exchange(false))
        {
            probe_inventory();
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

    std::atomic<bool> want_probe_{false};
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
