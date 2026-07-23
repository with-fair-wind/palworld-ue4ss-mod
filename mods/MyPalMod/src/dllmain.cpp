// MyPalMod — UE4SS C++ mod for Palworld 1.0.
//
//   Build:   cmake --preset ninja-msvc-x64 && cmake --build --preset ninja-msvc-x64
//   Deploy:  cmake --build --preset ninja-msvc-x64 --target deploy
//
// An in-game item/Pal/passive editor via the UE4SS GUI. Features are triggered from a
// floating ImGui window (select the "MyPalMod" tab). Game-interaction functions live in
// pal_game.hpp; this file holds the mod class, the GUI render callback, the request
// dispatch (on_update), and the ProcessEvent hook (viewed-Pal tracking).
//
// Thread model: ImGui renders on the GUI thread; it only stages requests (atomic flags
// + mutex-protected params). on_update runs on the game thread and does all Unreal
// reflection calls. Results flow back via mutex-protected caches.

#include <DynamicOutput/DynamicOutput.hpp>
#include <GUI/GUITab.hpp>
#include <Mod/CppUserModBase.hpp>
#include <UE4SSProgram.hpp>                          // UE4SS_ENABLE_IMGUI
#include <Unreal/CoreUObject/UObject/UnrealType.hpp> // FProperty (for viewed-Pal name read)
#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <imgui.h>

#include "item_database.h"
#include "pal_game.hpp"
#include "pal_skills.hpp"

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

using namespace RC;
using namespace RC::Unreal;
using pal_game::InvEntry;
using pal_game::PalEntry;

class MyPalMod final : public CppUserModBase
{
public:
    MyPalMod() : CppUserModBase()
    {
        ModName = STR("MyPalMod");
        ModVersion = STR("1.3.1");
        ModDescription = STR("In-game item/Pal/passive editor for Palworld 1.0");
        ModAuthors = STR("with-fair-wind");

        Output::send<LogLevel::Verbose>(STR("MyPalMod loaded (v1.3.1)\n"));

        register_tab(STR("MyPalMod"),
                     [](CppUserModBase* mod)
                     {
                         UE4SS_ENABLE_IMGUI()
                         auto* self = static_cast<MyPalMod*>(mod);
                         ImGui::TextUnformatted("A floating 'MyPalMod' window should be visible ->");
                         if (ImGui::Begin("MyPalMod v1.3", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
                         {
                             render_give_items(self);
                             ImGui::Separator();
                             render_item_browser(self);
                             ImGui::Separator();
                             render_inventory(self);
                             ImGui::Separator();
                             render_pal_editor(self);
                             ImGui::Separator();
                             if (ImGui::Button("Discover"))
                             {
                                 self->want_discover_.store(true);
                             }
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

        // Track which Pal the player is viewing (GetPassiveSkillList hook).
        fnGetPSL_ = UObjectGlobals::StaticFindObject<UFunction*>(
            nullptr, nullptr, STR("/Script/Pal.PalIndividualCharacterParameter:GetPassiveSkillList"));
        if (fnGetPSL_ != nullptr)
        {
            Hook::RegisterProcessEventPreCallback(
                [this](auto& /*info*/, UObject* Context, UFunction* Function, void* /*Params*/)
                {
                    if (Function == fnGetPSL_ && Context != nullptr)
                    {
                        lastViewedPal_.store(reinterpret_cast<uintptr_t>(Context));
                    }
                },
                Hook::FCallbackOptions{.OwnerModName = STR("MyPalMod"), .HookName = STR("PalViewTracker")});
            Output::send<LogLevel::Verbose>(STR("MyPalMod: PalViewTracker hook registered\n"));
        }
    }

    auto on_update() -> void override
    {
        // Cache the viewed Pal's species name when pointer changes.
        const uintptr_t viewed = lastViewedPal_.load();
        if (viewed != lastCachedPal_)
        {
            lastCachedPal_ = viewed;
            std::string name;
            if (viewed != 0)
            {
                UObject* pal = reinterpret_cast<UObject*>(viewed);
                if (FProperty* spProp = pal->GetPropertyByNameInChain(STR("SaveParameter")))
                {
                    if (FName* charId = spProp->ContainerPtrToValuePtr<FName>(pal))
                    {
                        const std::wstring w = charId->ToString();
                        name = std::string(w.begin(), w.end());
                    }
                }
            }
            {
                const std::lock_guard lock(inv_mutex_);
                viewedPalName_ = std::move(name);
            }
        }

        // Give items
        std::string item;
        int count = 0;
        bool doGive = false;
        {
            const std::lock_guard lock(req_mutex_);
            if (give_requested_.load())
            {
                give_requested_.store(false);
                item = give_item_;
                count = give_count_;
                doGive = true;
            }
        }
        if (doGive)
        {
            pal_game::give_items(item, static_cast<int32>(count));
            want_read_.store(true);
        }

        // Modify inventory count
        int32_t modSlot = 0;
        int32_t modCount = 0;
        bool doMod = false;
        {
            const std::lock_guard lock(req_mutex_);
            if (modify_requested_.load())
            {
                modify_requested_.store(false);
                modSlot = modify_slot_;
                modCount = modify_count_;
                doMod = true;
            }
        }
        if (doMod)
        {
            pal_game::set_slot_count(modSlot, modCount);
            want_read_.store(true);
        }

        // Read inventory
        if (want_read_.exchange(false))
        {
            auto fresh = pal_game::read_inventory();
            const std::lock_guard lock(inv_mutex_);
            if (selected_ >= static_cast<int>(fresh.size()))
            {
                selected_ = -1;
            }
            inv_cache_ = std::move(fresh);
        }

        // Scan items
        if (want_scan_items_.exchange(false))
        {
            auto fresh = pal_game::scan_all_items();
            const std::lock_guard lock(inv_mutex_);
            item_db_cache_ = std::move(fresh);
        }

        // Scan pals
        if (want_scan_pals_.exchange(false))
        {
            auto fresh = pal_game::scan_pals();
            const std::lock_guard lock(inv_mutex_);
            if (pal_selected_ >= static_cast<int>(fresh.size()))
            {
                pal_selected_ = -1;
            }
            pal_cache_ = std::move(fresh);
        }

        // Discover
        if (want_discover_.exchange(false))
        {
            pal_game::discover_objects();
        }
    }

private:
    static auto clamp(int v, int lo, int hi) -> int
    {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    // --- GUI render helpers (static, called from the register_tab lambda) ---

    static void render_give_items(MyPalMod* self)
    {
        ImGui::TextUnformatted("Give items");
        ImGui::InputText("Item ID", self->item_buf_, sizeof(self->item_buf_));
        ImGui::InputInt("Count", &self->count_input_);
        self->count_input_ = clamp(self->count_input_, 1, 9999);
        if (ImGui::Button("Give"))
        {
            const std::lock_guard lock(self->req_mutex_);
            self->give_item_ = self->item_buf_;
            self->give_count_ = self->count_input_;
            self->give_requested_ = true;
        }
    }

    static void render_item_browser(MyPalMod* self)
    {
        if (ImGui::Button("Scan game items"))
        {
            self->want_scan_items_.store(true);
        }
        ImGui::SameLine();
        ImGui::InputText("##search", self->search_buf_, sizeof(self->search_buf_));
        {
            const std::lock_guard lock(self->inv_mutex_);
            ImGui::TextDisabled("(%d items)",
                                self->item_db_cache_.empty() ? kBrowseItemCount
                                                             : static_cast<int>(self->item_db_cache_.size()));
        }
        ImGui::BeginChild("browser", ImVec2(380, 160), true);
        {
            std::string filter(self->search_buf_);
            for (auto& c : filter)
            {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            auto tryItem = [&](const char* raw)
            {
                std::string lower(raw);
                for (auto& c : lower)
                {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                if (!filter.empty() && lower.find(filter) == std::string::npos)
                {
                    return;
                }
                if (ImGui::Selectable(raw))
                {
                    const auto copyLen = std::min<std::size_t>(std::strlen(raw), sizeof(self->item_buf_) - 1);
                    std::memcpy(self->item_buf_, raw, copyLen);
                    self->item_buf_[copyLen] = '\0';
                }
            };
            const std::lock_guard lock(self->inv_mutex_);
            if (!self->item_db_cache_.empty())
            {
                for (const auto& item : self->item_db_cache_)
                {
                    tryItem(item.c_str());
                }
            }
            else
            {
                for (int bi = 0; bi < kBrowseItemCount; ++bi)
                {
                    tryItem(kBrowseItems[bi]);
                }
            }
        }
        ImGui::EndChild();
    }

    static void render_inventory(MyPalMod* self)
    {
        if (ImGui::Button("Refresh inventory"))
        {
            self->want_read_.store(true);
        }
        ImGui::SameLine();
        ImGui::TextUnformatted("(click an item to select, then set count)");
        {
            const std::lock_guard lock(self->inv_mutex_);
            ImGui::BeginChild("invlist", ImVec2(380, 220), true);
            for (int i = 0; i < static_cast<int>(self->inv_cache_.size()); ++i)
            {
                const auto& e = self->inv_cache_[i];
                const std::string label = e.item_id + "  x" + std::to_string(e.count) + " ##inv" + std::to_string(i);
                if (ImGui::Selectable(label.c_str(), self->selected_ == i))
                {
                    self->selected_ = i;
                    self->set_count_input_ = e.count;
                }
            }
            ImGui::EndChild();

            if (self->selected_ >= 0 && self->selected_ < static_cast<int>(self->inv_cache_.size()))
            {
                const auto& e = self->inv_cache_[self->selected_];
                ImGui::Text("Selected: %s (slot %d, x%d)", e.item_id.c_str(), static_cast<int>(e.slot_index), e.count);
                ImGui::InputInt("New count", &self->set_count_input_);
                self->set_count_input_ = clamp(self->set_count_input_, 0, 9999);
                if (ImGui::Button("Set count"))
                {
                    const std::lock_guard lock2(self->req_mutex_);
                    self->modify_slot_ = e.slot_index;
                    self->modify_count_ = self->set_count_input_;
                    self->modify_requested_ = true;
                }
            }
        }
    }

    static void render_pal_editor(MyPalMod* self)
    {
        if (!ImGui::CollapsingHeader("Pal editor"))
        {
            return;
        }

        // Currently Viewed Pal (auto-detected via hook)
        const uintptr_t vPtr = self->lastViewedPal_.load();
        if (vPtr != 0)
        {
            std::string vName;
            {
                const std::lock_guard lock(self->inv_mutex_);
                vName = self->viewedPalName_;
            }
            ImGui::TextColored(
                ImVec4(0.4F, 1.0F, 0.4F, 1.0F), "Currently Viewed: %s", vName.empty() ? "(reading...)" : vName.c_str());
        }
        else
        {
            ImGui::TextDisabled("View a Pal in-game (open Palbox/party) to auto-detect.");
        }
        ImGui::Separator();

        // Pal list (manual selection fallback)
        if (ImGui::Button("Scan Pals"))
        {
            self->want_scan_pals_.store(true);
        }
        ImGui::SameLine();
        {
            const std::lock_guard lock(self->inv_mutex_);
            ImGui::TextDisabled("(%d pals)", static_cast<int>(self->pal_cache_.size()));
        }
        ImGui::BeginChild("pallist", ImVec2(380, 140), true);
        {
            const std::lock_guard lock(self->inv_mutex_);
            for (int i = 0; i < static_cast<int>(self->pal_cache_.size()); ++i)
            {
                const std::string palLabel = self->pal_cache_[i].name + " ##pal" + std::to_string(i);
                if (ImGui::Selectable(palLabel.c_str(), self->pal_selected_ == i))
                {
                    self->pal_selected_ = i;
                }
            }
        }
        ImGui::EndChild();

        ImGui::TextDisabled("Active/passive skill controls are loading...");
    }

    // --- Members ---

    // UI state (GUI thread)
    char item_buf_[64] = "PalSphere_Tera";
    char search_buf_[64]{};
    int count_input_ = 10;
    int set_count_input_ = 0;
    int selected_ = -1;

    // Request handoff (GUI -> game thread)
    std::mutex req_mutex_;
    std::string give_item_;
    int give_count_ = 0;
    std::atomic<bool> give_requested_{false};
    int32_t modify_slot_ = 0;
    int32_t modify_count_ = 0;
    std::atomic<bool> modify_requested_{false};

    // Inventory + item DB cache (game thread writes, GUI thread reads)
    std::mutex inv_mutex_;
    std::vector<InvEntry> inv_cache_;
    std::vector<std::string> item_db_cache_;

    std::atomic<bool> want_read_{false};
    std::atomic<bool> want_discover_{false};
    std::atomic<bool> want_scan_items_{false};

    // Pal editor
    int pal_selected_ = -1;
    std::vector<PalEntry> pal_cache_;
    std::atomic<bool> want_scan_pals_{false};

    // Viewed-Pal auto-detection (via GetPassiveSkillList hook)
    std::atomic<uintptr_t> lastViewedPal_{0};
    UFunction* fnGetPSL_{};
    std::string viewedPalName_;
    uintptr_t lastCachedPal_{0};

    pal_skills::PalSkillGateway skillGateway_;
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
