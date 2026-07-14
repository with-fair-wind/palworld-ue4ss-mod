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

    // The property exists but is not a simple int (e.g. an array of slots). Extend here:
    //   CastField<FArrayProperty>  -> iterate the TArray and sum per-slot counts.
    //   CastField<FStructProperty> -> read struct sub-fields.
    //
    // Alternative: call a game UFunction that returns the count directly, e.g.
    //   auto* fn = UObjectGlobals::StaticFindObject<UFunction*>(
    //       nullptr, nullptr, STR("/Script/Pal.<Class>:<GetCountFunc>"));
    //   struct { /* in params */ } params{};
    //   inventory->ProcessEvent(fn, &params); // out params written back to `params`
    Output::send<LogLevel::Warning>(STR("ItemCount: '{}' is not an int property (more type info needed)\n"),
                                    kCountPropertyName);
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
        // Called every frame while the game runs. Keep it cheap.
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
