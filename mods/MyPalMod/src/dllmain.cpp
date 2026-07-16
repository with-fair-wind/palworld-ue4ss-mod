// MyPalMod — minimal UE4SS C++ mod skeleton for Palworld 1.0.
//
//   Build:   cmake --preset ninja-msvc-x64 && cmake --build --preset ninja-msvc-x64
//   Deploy:  cmake --build --preset ninja-msvc-x64 --target deploy
//
// Entry-point contract: UE4SS loads this DLL, calls start_mod() to construct the mod
// instance, and uninstall_mod() to destroy it. The lifecycle hooks (on_update,
// on_unreal_init) come from CppUserModBase.

#include <DynamicOutput/DynamicOutput.hpp>
#include <Mod/CppUserModBase.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp> // FIntProperty, FProperty, CastField
#include <Unreal/Hooks/Hooks.hpp>                    // RegisterProcessConsoleExecGlobalPreCallback
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include <map>
#include <string>
#include <string_view>

using namespace RC;
using namespace RC::Unreal;

namespace
{
// Broad keywords used to surface candidate Pal/item class names during discovery.
// Intentionally wide — the goal is to SEE which real Palworld class names exist.
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

// PLACEHOLDER for the item-count query (filled in once discovery reveals real names).
constexpr const TCHAR* kInventoryClassName = STR("PalPlayerInventoryData");
constexpr const TCHAR* kCountPropertyName = STR("StackCount");
constexpr const TCHAR* kConsoleCommand = STR("mypal.itemcount");

auto query_item_count() -> void
{
    UObject* inventory = UObjectGlobals::FindFirstOf(kInventoryClassName);
    if (inventory == nullptr)
    {
        Output::send<LogLevel::Warning>(STR("ItemCount: inventory class '{}' not found\n"), kInventoryClassName);
        return;
    }
    FProperty* property = inventory->GetPropertyByNameInChain(kCountPropertyName);
    if (property == nullptr)
    {
        Output::send<LogLevel::Warning>(
            STR("ItemCount: property '{}' not found on '{}'\n"), kCountPropertyName, kInventoryClassName);
        return;
    }
    if (auto* int_property = CastField<FIntProperty>(property))
    {
        const int32 count = int_property->GetPropertyValueInContainer(inventory);
        Output::send<LogLevel::Verbose>(STR("ItemCount: {} = {}\n"), kCountPropertyName, count);
        return;
    }
    Output::send<LogLevel::Warning>(STR("ItemCount: '{}' is not an int property\n"), kCountPropertyName);
}

// Discovery v2: scan every UObject once, build a histogram of class names that match
// any broad keyword, and log "[discover] <ClassName> (x<count>)". This reveals the REAL
// Palworld class names for the running version without guessing. Triggered once after a
// short delay (see on_update) so the player is in-game by then. May briefly pause the
// game (one-shot, full scan).
auto discover_objects() -> void
{
    Output::send<LogLevel::Warning>(STR("=== MyPalMod discovery v2: scanning UObjects ===\n"));
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
        ModVersion = STR("0.2.0");
        ModDescription = STR("Minimal UE4SS mod for Palworld 1.0");
        ModAuthors = STR("with-fair-wind");

        // Unique marker so you can confirm this build is loaded.
        Output::send<LogLevel::Verbose>(STR("MyPalMod loaded (discovery v2)\n"));
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
                    query_item_count();
                }
            },
            Hook::FCallbackOptions{.OwnerModName = STR("MyPalMod"), .HookName = STR("ItemCountConsoleCmd")});
    }

    auto on_update() -> void override
    {
        // Run the one-shot discovery ~20s after the game starts ticking (by then the
        // player is usually in-game). Time-based, not gated on any class name guess.
        if (!discovery_done_ && ++tick_counter_ >= 1200)
        {
            discovery_done_ = true;
            discover_objects();
        }
    }

private:
    bool discovery_done_{false};
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
