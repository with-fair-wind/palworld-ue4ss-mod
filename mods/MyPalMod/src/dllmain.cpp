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

#include <string_view> // std::wstring_view

using namespace RC;
using namespace RC::Unreal;

namespace
{
// ============================================================================
// PLACEHOLDERS — replace after discovering the real Palworld symbols.
//   How to discover them:
//     - Live Property Viewer (UE4SS in-game GUI), or
//     - SDK dump: enable GenerateSDK/DumpAllObjects in UE4SS-settings.ini, then
//       grep the dump under <game>\Pal\Binaries\Win64\dumps\ for "Inventory"/"Item"/
//       "Slot"/"Count". The names below are EXAMPLES only and will not resolve until
//       you fill in the real ones — the code stays safe (logs "not found") until then.
// ============================================================================

// The UClass that holds the player's items (example name only).
constexpr const TCHAR* kInventoryClassName = STR("PalPlayerInventoryData");
// An int32 property on that class representing a stack/total count (example only).
constexpr const TCHAR* kCountPropertyName = STR("StackCount");
// The console command that triggers the query (typed in the UE4SS console in-game).
constexpr const TCHAR* kConsoleCommand = STR("mypal.itemcount");

// Demonstrates the property-reflection route: locate an object by class name, then
// read an int32 property by name. Compiles and runs as-is (a safe no-op that logs
// "not found" until the real Palworld names are filled in above).
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

    Output::send<LogLevel::Warning>(STR("ItemCount: '{}' is not an int property (more type info needed)\n"),
                                    kCountPropertyName);
}

// Discovery scan: walk every UObject once and log the full name of any object whose
// name hints at Pal/items. This reveals the REAL Palworld class names for the current
// game version (so the placeholders above can be filled in). Runs once automatically
// when the player is in-game — see on_update(). Uses only ForEachUObject + GetFullName,
// both verified to compile. May briefly pause the game (one-shot, full scan).
auto discover_objects() -> void
{
    Output::send<LogLevel::Warning>(STR("=== MyPalMod discovery: scanning UObjects (may pause briefly) ===\n"));
    int matched = 0;
    constexpr int kMaxLogged = 300;
    UObjectGlobals::ForEachUObject(
        [&](UObject* obj, int32_t, int32_t) -> LoopAction
        {
            const auto full = obj->GetFullName();
            if (full.find(STR("PalIndividualCharacter")) != std::wstring::npos ||
                full.find(STR("Inventory")) != std::wstring::npos ||
                full.find(STR("ItemContainer")) != std::wstring::npos ||
                full.find(STR("PalItem")) != std::wstring::npos || full.find(STR("PassiveSkill")) != std::wstring::npos)
            {
                Output::send<LogLevel::Warning>(STR("[discover] {}\n"), full);
                if (++matched >= kMaxLogged)
                {
                    return LoopAction::Break;
                }
            }
            return LoopAction::Continue;
        });
    Output::send<LogLevel::Warning>(STR("=== discovery done: {} matched ===\n"), matched);
}
} // namespace

class MyPalMod final : public CppUserModBase
{
public:
    MyPalMod() : CppUserModBase()
    {
        ModName = STR("MyPalMod");
        ModVersion = STR("0.1.0");
        ModDescription = STR("Minimal UE4SS mod for Palworld 1.0");
        ModAuthors = STR("with-fair-wind");

        // Propagated to the log file, the console, and the GUI console.
        Output::send<LogLevel::Verbose>(STR("MyPalMod loaded\n"));
    }

    ~MyPalMod() override = default;

    // Fires once after Unreal is initialized — the Unreal namespace is safe to use
    // here and everywhere after.
    auto on_unreal_init() -> void override
    {
        // Sanity check: the Unreal reflection API is reachable.
        if (const auto object =
                UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/CoreUObject.Object")))
        {
            Output::send<LogLevel::Verbose>(STR("Object Name: {}\n"), object->GetFullName());
        }

        // Register an on-demand console command. In-game, open the UE4SS console and
        // type `mypal.itemcount` to run the query. (C++ keybinds aren't exposed in this
        // UE4SS revision; a console command is the most reliable on-demand trigger.)
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
        // One-shot discovery: once the player is in-game (a Pal exists), dump the names
        // of all Pal/item-related objects to the log. Send the [discover] lines back so
        // the real class names can be wired into the placeholders above.
        // Throttled — FindFirstOf is a full scan, so only check ~once per second.
        if (discovery_done_ || (++tick_counter_ % 60) != 0)
        {
            return;
        }
        if (UObjectGlobals::FindFirstOf(STR("PalIndividualCharacter")) != nullptr)
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
