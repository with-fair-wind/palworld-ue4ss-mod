// MyPalMod — UE4SS C++ mod for Palworld 1.0.
//
//   Build:   cmake --preset ninja-msvc-x64 && cmake --build --preset ninja-msvc-x64
//   Deploy:  cmake --build --preset ninja-msvc-x64 --target deploy
//
// Triggered via a custom "MyPalMod" tab in the UE4SS GUI window (the separate window
// that opens alongside the game — NOT the F10 in-game console, which is broken by
// Palworld's ambiguous ConsoleManager signature). Button clicks hand the request to
// on_update (game thread) via a mutex / atomic; UE reflection is not thread-safe, so
// all actual game-function calls happen there.
//
//   Give items    - type an item ID + count, click Give -> AddItem_ServerInternal
//   Discover      - scan all UObjects, log a histogram of Pal/item-related classes
//
// Item IDs are the BARE Palworld StaticItemId (FName): PalSphere, PalSphere_Tera,
// Stone, Wood, Money, ... Full list: github.com/KURAMAAA0/PalModding ItemIDs.txt.
//
// Discovered Palworld API (from UHTHeaderDump, Pal module):
//   Items : UPalPlayerInventoryData::AddItem_ServerInternal(FName StaticItemId, int32 Count,
//           bool IsAssignPassive, float LogDelay, bool bNotifyLog) -> EPalItemOperationResult
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
#include <cstring>
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

// Add `count` of `item_id` to the player's inventory via UPalPlayerInventoryData::AddItem_ServerInternal.
// `item_id` is a narrow (UTF-8/ASCII) string from the UI; converted to FName internally.
// MUST be called on the game thread.
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
    Output::send<LogLevel::Warning>(STR("give_items: inventory instance = {}\n"), inventory->GetFullName());

    UFunction* fn = UObjectGlobals::StaticFindObject<UFunction*>(
        nullptr, nullptr, STR("/Script/Pal.PalPlayerInventoryData:AddItem_ServerInternal"));
    if (fn == nullptr)
    {
        Output::send<LogLevel::Warning>(STR("give_items: AddItem_ServerInternal UFunction not found\n"));
        return;
    }

    // Narrow (UI) -> wide (FName). Item IDs are ASCII, so a simple char->wchar_t widen is enough.
    std::wstring wide(item_id.begin(), item_id.end());

    // Param layout must match the UFunction:
    //   { FName StaticItemId; int32 Count; bool IsAssignPassive; float LogDelay; bool bNotifyLog; }
    //   + return EPalItemOperationResult (int32-sized space, output-only).
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
    params.StaticItemId = FName(wide.c_str());
    params.Count = count;
    params.IsAssignPassive = false;
    params.LogDelay = 0.0F;
    params.bNotifyLog = false;

    inventory->ProcessEvent(fn, &params);
    // Convert item_id back to wide only for logging (Output::send uses wide).
    Output::send<LogLevel::Warning>(
        STR("give_items: called AddItem_ServerInternal('{}', x{}) -> result={}\n"), wide, params.Count, params.Result);
}

// Discovery: histogram of class names matching narrow keywords. Reveals real Palworld
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
        ModVersion = STR("0.6.0");
        ModDescription = STR("UE4SS C++ mod for Palworld 1.0");
        ModAuthors = STR("with-fair-wind");

        Output::send<LogLevel::Verbose>(STR("MyPalMod loaded (v0.6.0 item UI)\n"));

        // Register a tab in the UE4SS GUI window. The render callback runs on the GUI
        // thread, so it only stages a request (under a mutex); the actual game-function
        // calls run on the game thread in on_update.
        // NOTE: ImGui uses narrow char*, so the item-id input is narrow and widened later.
        register_tab(STR("MyPalMod"),
                     [](CppUserModBase* mod)
                     {
                         UE4SS_ENABLE_IMGUI()
                         auto* self = static_cast<MyPalMod*>(mod);

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
                         ImGui::SameLine();
                         if (ImGui::Button("Discover"))
                         {
                             self->want_discover_.store(true);
                         }

                         ImGui::Separator();
                         ImGui::TextWrapped("Item IDs (bare): PalSphere, PalSphere_Tera, PalSphere_Master, "
                                            "PalSphere_Legend, Stone, Wood, Money, AncientParts2, ...  "
                                            "Full list: github.com/KURAMAAA0/PalModding ItemIDs.txt");
                         ImGui::TextUnformatted("Load into a save first. Results -> UE4SS Console tab.");
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
        // Game thread: safe to call game functions here.
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

    // Handoff to game thread (protected by give_mutex_)
    std::mutex give_mutex_;
    std::string give_item_;
    int give_count_ = 0;
    bool give_requested_ = false;

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
