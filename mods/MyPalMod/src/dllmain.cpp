// MyPalMod — UE4SS C++ mod for Palworld 1.0.
//
//   Build:   cmake --preset ninja-msvc-x64 && cmake --build --preset ninja-msvc-x64
//   Deploy:  cmake --build --preset ninja-msvc-x64 --target deploy
//
// Discovered Palworld API (from UHTHeaderDump, Pal module):
//   Items : UPalPlayerInventoryData::RequestAddItem_ForDebug(FName StaticItemId, int32 Count, bool IsAssignPassive)
//   Pals  : UPalIndividualCharacterParameter — AddPassiveSkill/RemovePassiveSkill/GetPassiveSkillList,
//           and SaveParameter (FPalIndividualCharacterSaveParameter) with PassiveSkillList (TArray<FName>),
//           Talent_HP/Melee/Shot/Defense (uint8 IVs), Rank, Level, Rank_HP/Attack/Defence/CraftSpeed.

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
// ============================================================================
// TWEAK THESE — item give feature. kItemId must be a real Palworld StaticItemId
// (FName). Find valid IDs by grepping UHTHeaderDump for item data tables, e.g.
// PalStaticItemDataStruct.h / EPalItemType*.h, or watch the inventory in Live View.
// ============================================================================
constexpr const TCHAR* kItemId = STR("ItemName_Stone");
constexpr int32 kItemCount = 10;

// Broad keywords for the discovery histogram (surfaces real class names).
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

constexpr const TCHAR* kConsoleCommand = STR("mypal.itemcount");
constexpr const TCHAR* kInventoryClassName = STR("PalPlayerInventoryData");
constexpr const TCHAR* kCountPropertyName = STR("StackCount");

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

// Discovery: histogram of class names matching broad keywords. Reveals the real
// Palworld class names for the running version. One-shot, full scan.
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
        ModVersion = STR("0.3.0");
        ModDescription = STR("UE4SS C++ mod for Palworld 1.0");
        ModAuthors = STR("with-fair-wind");

        Output::send<LogLevel::Verbose>(STR("MyPalMod loaded (v0.3 give-items)\n"));
    }

    ~MyPalMod() override = default;

    auto on_unreal_init() -> void override
    {
        if (const auto object =
                UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/CoreUObject.Object")))
        {
            Output::send<LogLevel::Verbose>(STR("Object Name: {}\n"), object->GetFullName());
        }

        Hook::RegisterProcessConsoleExecGlobalPreCallback(
            [](auto& /*info*/, UObject* /*context*/, const TCHAR* cmd, FOutputDevice& /*ar*/, UObject* /*executor*/)
            {
                if (std::wstring_view(cmd).starts_with(kConsoleCommand))
                {
                    // placeholder: read an int property (fills in real names later)
                    Output::send<LogLevel::Warning>(STR("mypal.itemcount: (placeholder) inventory='{}'\n"),
                                                    kInventoryClassName);
                }
            },
            Hook::FCallbackOptions{.OwnerModName = STR("MyPalMod"), .HookName = STR("ItemCountConsoleCmd")});
    }

    auto on_update() -> void override
    {
        // One-shot discovery ~20s after gameplay starts.
        if (!discovery_done_ && ++tick_counter_ >= 1200)
        {
            discovery_done_ = true;
            discover_objects();
        }
        // Give items a bit later (~25s) — one shot per session. Edit kItemId/kItemCount
        // at the top of the file, rebuild, redeploy, and load a save to test again.
        if (!give_done_ && tick_counter_ >= 1500)
        {
            give_done_ = true;
            give_items();
        }
    }

private:
    bool discovery_done_{false};
    bool give_done_{false};
    int tick_counter_{0};
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
