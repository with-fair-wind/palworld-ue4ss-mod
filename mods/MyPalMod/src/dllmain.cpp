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
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

using namespace RC;
using namespace RC::Unreal;

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
        if (const auto object =
                UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/CoreUObject.Object")))
        {
            // Output::send is std::format-backed, so {} placeholders are used.
            Output::send<LogLevel::Verbose>(STR("Object Name: {}\n"), object->GetFullName());
        }
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
