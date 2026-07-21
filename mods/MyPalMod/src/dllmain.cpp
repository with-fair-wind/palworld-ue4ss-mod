// MyPalMod — UE4SS C++ mod for Palworld 1.0.
//
//   Build:   cmake --preset ninja-msvc-x64 && cmake --build --preset ninja-msvc-x64
//   Deploy:  cmake --build --preset ninja-msvc-x64 --target deploy
//
// Features are triggered via the in-game console (opens with F10 / Tilde via the
// built-in ConsoleEnablerMod). Load into a save first, open the console, then type:
//   mypal.discover   - scan all UObjects, log a histogram of Pal/item-related classes
//   mypal.give       - add kItemCount of item kItemId to the player's inventory
//
// Discovered Palworld API (from UHTHeaderDump, Pal module):
//   Items : UPalPlayerInventoryData::RequestAddItem_ForDebug(FName StaticItemId, int32 Count, bool IsAssignPassive)
//   Pals  : UPalIndividualCharacterParameter — AddPassiveSkill/RemovePassiveSkill/GetPassiveSkillList,
//           SaveParameter (FPalIndividualCharacterSaveParameter) with PassiveSkillList (TArray<FName>),
//           Talent_HP/Melee/Shot/Defense (uint8 IVs), Rank, Level.

#include <DynamicOutput/DynamicOutput.hpp>
#include <Mod/CppUserModBase.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp> // FIntProperty, FProperty, CastField
#include <Unreal/Hooks/Hooks.hpp>                    // RegisterProcessConsoleExecGlobalPreCallback
#include <Unreal/NameTypes.hpp>                      // FName
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

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

// Broad keywords for the discovery histogram (surfaces real class names + live counts).
constexpr std::wstring_view kDiscoveryKeywords[] = {
    L"Pal",
    L"Item",
    L"Invent",
    L"Character",
    L"Player",
    L"Container",
    L"Slot",
    L"Skill",
    L"Passive",
    L"Talent",
    L"Rank",
    L"Stat",
    L"Status",
    L"Otter",
    L"Wallet",
    L"Box",
    L"Storage",
};

// Give items by calling UPalPlayerInventoryData::RequestAddItem_ForDebug via ProcessEvent.
// Param layout must match the UFunction: { FName StaticItemId; int32 Count; bool IsAssignPassive; }.
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

    UFunction* fn = UObjectGlobals::StaticFindObject<UFunction*>(
        nullptr, nullptr, STR("/Script/Pal.PalPlayerInventoryData:RequestAddItem_ForDebug"));
    if (fn == nullptr)
    {
        Output::send<LogLevel::Warning>(STR("give_items: RequestAddItem_ForDebug UFunction not found\n"));
        return;
    }

    struct FRequestAddItemParams
    {
        FName StaticItemId;
        int32_t Count;
        bool IsAssignPassive;
    };
    FRequestAddItemParams params{};
    params.StaticItemId = FName(kItemId);
    params.Count = kItemCount;
    params.IsAssignPassive = false;

    inventory->ProcessEvent(fn, &params);
    Output::send<LogLevel::Warning>(
        STR("give_items: called RequestAddItem_ForDebug('{}', x{})\n"), kItemId, params.Count);
}

// Discovery: histogram of class names matching broad keywords. Reveals the real Palworld
// class names + how many LIVE instances exist right now. Full scan (may briefly pause).
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
        ModVersion = STR("0.4.0");
        ModDescription = STR("UE4SS C++ mod for Palworld 1.0");
        ModAuthors = STR("with-fair-wind");

        Output::send<LogLevel::Verbose>(STR("MyPalMod loaded (v0.4 console cmds)\n"));
    }

    ~MyPalMod() override = default;

    auto on_unreal_init() -> void override
    {
        if (const auto object =
                UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/CoreUObject.Object")))
        {
            Output::send<LogLevel::Verbose>(STR("Object Name: {}\n"), object->GetFullName());
        }

        // Console commands: open the in-game console (F10 / Tilde) and type the command.
        Hook::RegisterProcessConsoleExecGlobalPreCallback(
            [](auto& /*info*/, UObject* /*context*/, const TCHAR* cmd, FOutputDevice& /*ar*/, UObject* /*executor*/)
            {
                const std::wstring_view c(cmd);
                if (c.starts_with(STR("mypal.discover")))
                {
                    discover_objects();
                }
                else if (c.starts_with(STR("mypal.give")))
                {
                    give_items();
                }
            },
            Hook::FCallbackOptions{.OwnerModName = STR("MyPalMod"), .HookName = STR("MyPalCommands")});
    }

    auto on_update() -> void override
    {
        // Features are console-triggered (see on_unreal_init). The old timer triggers
        // fired at the main menu, before live in-game instances existed.
    }
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
