// MyPalMod — UE4SS C++ mod for Palworld 1.0.
//
//   Build:   cmake --preset ninja-msvc-x64 && cmake --build --preset ninja-msvc-x64
//   Deploy:  cmake --build --preset ninja-msvc-x64 --target deploy
//
// Triggered via a custom tab in the UE4SS GUI window (the separate window that opens
// alongside the game — NOT the F10 in-game console, which is broken by Palworld's
// ambiguous ConsoleManager signature). Open the UE4SS GUI, go to the "MyPalMod" tab,
// click a button. Button clicks set atomic flags; the actual game-function calls run
// on the game thread in on_update (safe — UE reflection is not thread-safe).
//
//   "Give items"     - add kItemCount of kItemId to the player's inventory
//   "Discover"       - scan all UObjects, log a histogram of Pal/item-related classes
//
// Discovered Palworld API (from UHTHeaderDump, Pal module):
//   Items : UPalPlayerInventoryData::RequestAddItem_ForDebug(FName StaticItemId, int32 Count, bool IsAssignPassive)
//   Pals  : UPalIndividualCharacterParameter — AddPassiveSkill/RemovePassiveSkill/GetPassiveSkillList,
//           SaveParameter (FPalIndividualCharacterSaveParameter) with PassiveSkillList (TArray<FName>),
//           Talent_HP/Melee/Shot/Defense (uint8 IVs), Rank, Level.

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
#include <string>
#include <string_view>

using namespace RC;
using namespace RC::Unreal;

namespace
{
// Item give feature. kItemId is a Palworld StaticItemId (FName): the BARE item name.
// Verified list: github.com/KURAMAAA0/PalModding ItemIDs.txt. Examples: Stone, Wood,
// PalSphere, PalSphere_Tera, PalSphere_Master, PalSphere_Legend, Money, AncientParts2.
constexpr const TCHAR* kItemId = STR("PalSphere_Tera");
constexpr int32 kItemCount = 10;
constexpr const TCHAR* kInventoryClassName = STR("PalPlayerInventoryData");

// Narrow filter — only the classes we actually need to see, so the output is short and
// not truncated before the relevant "P" section. Surfaces live instance counts.
constexpr std::wstring_view kDiscoveryKeywords[] = {
    L"Inventory",
    L"IndividualCharacter",
    L"ItemContainer",
    L"Otomo",
    L"PalCharacterContainer",
};

// Give items by calling UPalPlayerInventoryData::AddItem_ServerInternal via ProcessEvent.
// RequestAddItem_ForDebug was a no-op on the live instance (likely disabled in Shipping);
// AddItem_ServerInternal is the real server-side add (single-player host == server).
// Param layout: { FName StaticItemId; int32 Count; bool IsAssignPassive; float LogDelay;
// bool bNotifyLog; } + return EPalItemOperationResult. MUST be called on the game thread.
auto give_items() -> void
{
    Output::send<LogLevel::Warning>(STR("=== MyPalMod give_items: start ===\n"));

    UObject* inventory = UObjectGlobals::FindFirstOf(kInventoryClassName);
    if (inventory == nullptr)
    {
        Output::send<LogLevel::Warning>(STR("give_items: '{}' instance not found (not in-game?)\n"),
                                        kInventoryClassName);
        return;
    }
    // Log the full name so we can tell whether FindFirstOf returned a real instance or
    // just the CDO (class default object) — a CDO looks like "Default__PalPlayerInventoryData".
    Output::send<LogLevel::Warning>(STR("give_items: inventory instance = {}\n"), inventory->GetFullName());

    UFunction* fn = UObjectGlobals::StaticFindObject<UFunction*>(
        nullptr, nullptr, STR("/Script/Pal.PalPlayerInventoryData:AddItem_ServerInternal"));
    if (fn == nullptr)
    {
        Output::send<LogLevel::Warning>(STR("give_items: AddItem_ServerInternal UFunction not found\n"));
        return;
    }

    struct FAddItemServerInternalParams
    {
        FName StaticItemId;
        int32_t Count;
        bool IsAssignPassive;
        float LogDelay;
        bool bNotifyLog;
        int32_t Result; // EPalItemOperationResult (out)
    };
    FAddItemServerInternalParams params{};
    params.StaticItemId = FName(kItemId);
    params.Count = kItemCount;
    params.IsAssignPassive = false;
    params.LogDelay = 0.0f;
    params.bNotifyLog = false;

    inventory->ProcessEvent(fn, &params);
    Output::send<LogLevel::Warning>(STR("give_items: called AddItem_ServerInternal('{}', x{}) -> result={}\n"),
                                    kItemId,
                                    params.Count,
                                    params.Result);
}

// Discovery: histogram of class names matching broad keywords. Reveals real Palworld
// class names + how many LIVE instances exist right now. Full scan (may briefly pause).
// MUST be called on the game thread.
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
        ModVersion = STR("0.5.2");
        ModDescription = STR("UE4SS C++ mod for Palworld 1.0");
        ModAuthors = STR("with-fair-wind");

        Output::send<LogLevel::Verbose>(STR("MyPalMod loaded (v0.5.2 AddItem_ServerInternal)\n"));

        // Register a tab in the UE4SS GUI window. The render callback runs on the GUI
        // thread, so it only sets atomic flags; the actual work runs on the game thread
        // in on_update (UE reflection is not thread-safe).
        // NOTE: ImGui uses narrow char*, so UI labels are narrow string literals.
        register_tab(STR("MyPalMod"),
                     [](CppUserModBase* mod)
                     {
                         UE4SS_ENABLE_IMGUI()
                         auto* self = static_cast<MyPalMod*>(mod);
                         ImGui::TextUnformatted("MyPalMod v0.5");
                         ImGui::Separator();
                         if (ImGui::Button("Give items (PalSphere_Tera x10)"))
                         {
                             self->want_give_.store(true);
                         }
                         ImGui::SameLine();
                         if (ImGui::Button("Discover (scan objects)"))
                         {
                             self->want_discover_.store(true);
                         }
                         ImGui::Separator();
                         ImGui::TextWrapped("Load into a save first. Actions run on the next game tick. "
                                            "Results are printed to the UE4SS Console tab.");
                     });
    }

    ~MyPalMod() override = default;

    auto on_unreal_init() -> void override
    {
        // Sanity check: the Unreal reflection API is reachable.
        if (const auto object =
                UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/CoreUObject.Object")))
        {
            Output::send<LogLevel::Verbose>(STR("Object Name: {}\n"), object->GetFullName());
        }
    }

    auto on_update() -> void override
    {
        // Game thread: safe to call game functions here. Flags are set by the GUI thread.
        if (want_give_.exchange(false))
        {
            give_items();
        }
        if (want_discover_.exchange(false))
        {
            discover_objects();
        }
    }

private:
    std::atomic<bool> want_give_{false};
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
