// PalworldEditor — UE4SS C++ mod for Palworld 1.0.
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

#include <atomic>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include <DynamicOutput/DynamicOutput.hpp>
#include <GUI/GUITab.hpp>
#include <Mod/CppUserModBase.hpp>
#include <UE4SSProgram.hpp>                           // UE4SS_ENABLE_IMGUI
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>  // FProperty (for viewed-Pal name read)
#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <imgui.h>

#include <game/pal_game.hpp>
#include <skills/pal_skills.hpp>
#include <support/text_encoding.hpp>

using namespace RC;
using namespace RC::Unreal;
using pal_game::InvEntry;
using pal_game::PalEntry;

class MyPalMod final : public CppUserModBase {
public:
    MyPalMod() : CppUserModBase() {
        ModName = STR("MyPalMod");
        ModVersion = STR("1.4.0");
        ModDescription = STR("In-game item and active/passive Pal skill editor for Palworld 1.0");
        ModAuthors = STR("with-fair-wind");

        Output::send<LogLevel::Verbose>(STR("MyPalMod loaded (v1.4.0)\n"));

        register_tab(STR("MyPalMod"), [](CppUserModBase* mod) {
            UE4SS_ENABLE_IMGUI()
            auto* self = static_cast<MyPalMod*>(mod);
            ImGui::TextUnformatted("A floating 'MyPalMod' window should be visible ->");
            if (ImGui::Begin("MyPalMod v1.4.0", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                render_give_items(self);
                ImGui::Separator();
                render_item_browser(self);
                ImGui::Separator();
                render_inventory(self);
                ImGui::Separator();
                render_pal_editor(self);
                ImGui::Separator();
                if (ImGui::Button("Discover")) {
                    self->want_discover_.store(true);
                }
            }
            ImGui::End();
        });
    }

    ~MyPalMod() override = default;

    auto on_unreal_init() -> void override {
        if (const auto object = UObjectGlobals::StaticFindObject<UObject*>(
                nullptr, nullptr, STR("/Script/CoreUObject.Object"))) {
            Output::send<LogLevel::Verbose>(STR("Object Name: {}\n"), object->GetFullName());
        }

        // Track which Pal the player is viewing (GetPassiveSkillList hook).
        fnGetPSL_ = UObjectGlobals::StaticFindObject<UFunction*>(
            nullptr, nullptr,
            STR("/Script/Pal.PalIndividualCharacterParameter:GetPassiveSkillList"));
        if (fnGetPSL_ != nullptr) {
            Hook::RegisterProcessEventPreCallback(
                [this](auto& /*info*/, UObject* Context, UFunction* Function, void* /*Params*/) {
                    if (Function == fnGetPSL_ && Context != nullptr &&
                        !suppressViewTracking_.load()) {
                        lastViewedPal_.store(reinterpret_cast<uintptr_t>(Context));
                    }
                },
                Hook::FCallbackOptions{.OwnerModName = STR("MyPalMod"),
                                       .HookName = STR("PalViewTracker")});
            Output::send<LogLevel::Verbose>(STR("MyPalMod: PalViewTracker hook registered\n"));
        }

        want_scan_items_.store(true);
    }

    auto on_update() -> void override {
        // Cache the viewed Pal's species name when pointer changes.
        const uintptr_t viewed = lastViewedPal_.load();
        if (viewed != lastCachedPal_) {
            lastCachedPal_ = viewed;
            std::string name;
            if (skillGateway_.is_valid(viewed)) {
                UObject* pal = reinterpret_cast<UObject*>(viewed);
                if (FProperty* spProp = pal->GetPropertyByNameInChain(STR("SaveParameter"))) {
                    if (FName* charId = spProp->ContainerPtrToValuePtr<FName>(pal)) {
                        const std::wstring w = charId->ToString();
                        name = text_encoding::to_utf8(w);
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
            if (give_requested_.load()) {
                give_requested_.store(false);
                item = give_item_;
                count = give_count_;
                doGive = true;
            }
        }
        if (doGive) {
            pal_game::give_items(item, static_cast<int32>(count));
            want_read_.store(true);
        }

        // Modify inventory count
        int32_t modSlot = 0;
        int32_t modCount = 0;
        bool doMod = false;
        {
            const std::lock_guard lock(req_mutex_);
            if (modify_requested_.load()) {
                modify_requested_.store(false);
                modSlot = modify_slot_;
                modCount = modify_count_;
                doMod = true;
            }
        }
        if (doMod) {
            pal_game::set_slot_count(modSlot, modCount);
            want_read_.store(true);
        }

        // Read inventory
        if (want_read_.exchange(false)) {
            auto fresh = pal_game::read_inventory();
            const std::lock_guard lock(inv_mutex_);
            if (selected_ >= static_cast<int>(fresh.size())) {
                selected_ = -1;
            }
            inv_cache_ = std::move(fresh);
        }

        // Scan items
        if (want_scan_items_.exchange(false)) {
            auto fresh = pal_game::scan_all_items();
            const std::lock_guard lock(inv_mutex_);
            item_db_cache_ = std::move(fresh);
        }

        // Scan pals
        if (want_scan_pals_.exchange(false)) {
            auto fresh = pal_game::scan_pals();
            const std::lock_guard lock(inv_mutex_);
            if (pal_selected_ >= static_cast<int>(fresh.size())) {
                pal_selected_ = -1;
            }
            pal_cache_ = std::move(fresh);
        }

        std::optional<skill_editor::SkillEditResult> editResult;
        if (auto request = skillQueue_.try_pop()) {
            {
                const std::lock_guard lock(skillSnapshotMutex_);
                skillSnapshot_.pending = true;
            }
            ViewTrackingGuard guard(suppressViewTracking_);
            editResult = skill_editor::execute_skill_edit(skillGateway_, *request);
        }

        const auto resolved = resolve_skill_target();
        const bool targetChanged = resolved.target != lastSkillTarget_;
        lastSkillTarget_ = resolved.target;

        std::optional<skill_editor::SkillCatalogSnapshot> refreshedCatalog;
        if (resolved.target != 0 && wantRefreshSkillCatalog_.exchange(false)) {
            skill_editor::SkillCatalogSnapshot previous;
            {
                const std::lock_guard lock(skillSnapshotMutex_);
                previous = skillSnapshot_.catalog;
            }
            ViewTrackingGuard guard(suppressViewTracking_);
            refreshedCatalog = skill_editor::with_catalog_fallback(
                previous, skillGateway_.load_catalog(resolved.target));
        }

        std::optional<skill_editor::SkillState> refreshedState;
        if (resolved.target != 0 && (targetChanged || editResult.has_value())) {
            ViewTrackingGuard guard(suppressViewTracking_);
            refreshedState = skillGateway_.read_state(resolved.target);
        }

        {
            const std::lock_guard lock(skillSnapshotMutex_);
            skillSnapshot_.target = resolved.target;
            skillSnapshot_.source = resolved.source;
            skillSnapshot_.palName = resolved.name;
            skillSnapshot_.pending = skillQueue_.size() != 0;
            if (refreshedCatalog.has_value()) {
                skillSnapshot_.catalog = std::move(*refreshedCatalog);
            }
            if (refreshedState.has_value()) {
                skillSnapshot_.state = std::move(*refreshedState);
            } else if (resolved.target == 0) {
                skillSnapshot_.state = {};
            }
            if (editResult.has_value()) {
                skillSnapshot_.lastResult = editResult->message;
            }
        }

        // Discover
        if (want_discover_.exchange(false)) {
            pal_game::discover_objects();
        }
    }

private:
    enum class SkillTargetSource {
        none,
        viewed,
        selected,
    };

    struct ResolvedSkillTarget {
        skill_editor::SkillTarget target{};
        SkillTargetSource source{SkillTargetSource::none};
        std::string name;
    };

    struct SkillEditorSnapshot {
        skill_editor::SkillTarget target{};
        SkillTargetSource source{SkillTargetSource::none};
        std::string palName;
        skill_editor::SkillState state;
        skill_editor::SkillCatalogSnapshot catalog;
        std::string lastResult;
        bool pending{};
    };

    class ViewTrackingGuard {
    public:
        explicit ViewTrackingGuard(std::atomic<bool>& flag) : flag_(flag) {
            flag_.store(true);
        }

        ~ViewTrackingGuard() {
            flag_.store(false);
        }

        ViewTrackingGuard(const ViewTrackingGuard&) = delete;
        auto operator=(const ViewTrackingGuard&) -> ViewTrackingGuard& = delete;

    private:
        std::atomic<bool>& flag_;
    };

    static auto clamp(int v, int lo, int hi) -> int {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    auto resolve_skill_target() -> ResolvedSkillTarget {
        const auto viewed = lastViewedPal_.load();
        if (skillGateway_.is_valid(viewed)) {
            std::string name;
            {
                const std::lock_guard lock(inv_mutex_);
                name = viewedPalName_;
            }
            return {.target = viewed, .source = SkillTargetSource::viewed, .name = std::move(name)};
        }

        UObject* selected = nullptr;
        std::string name;
        {
            const std::lock_guard lock(inv_mutex_);
            if (pal_selected_ >= 0 && pal_selected_ < static_cast<int>(pal_cache_.size())) {
                selected = pal_cache_[pal_selected_].ptr;
                name = pal_cache_[pal_selected_].name;
            }
        }
        const auto selectedTarget = reinterpret_cast<skill_editor::SkillTarget>(selected);
        if (skillGateway_.is_valid(selectedTarget)) {
            return {.target = selectedTarget,
                    .source = SkillTargetSource::selected,
                    .name = std::move(name)};
        }
        return {};
    }

    static auto find_skill_label(const std::vector<skill_editor::SkillOption>& options,
                                 const std::string_view id) -> std::string {
        const auto found = std::ranges::find(options, id, &skill_editor::SkillOption::id);
        return found == options.end() ? std::string(id) : skill_editor::skill_label(*found);
    }

    static void reset_skill_editor_ui(MyPalMod* self) {
        self->passiveEditIndex_ = -1;
        self->activeEditSlot_ = -1;
        self->passiveChoice_.reset();
        self->activeChoice_.reset();
        self->passiveSearch_[0] = '\0';
        self->activeSearch_[0] = '\0';
    }

    static auto render_skill_picker(const char* id,
                                    const std::vector<skill_editor::SkillOption>& options,
                                    const std::unordered_set<std::string>& excludedIds,
                                    char* search, const std::size_t searchSize,
                                    std::optional<skill_editor::SkillOption>& selected) -> bool {
        const std::string preview =
            selected.has_value() ? skill_editor::skill_label(*selected) : "请选择技能";
        bool changed = false;
        if (ImGui::BeginCombo(id, preview.c_str())) {
            ImGui::SetNextItemWidth(340.0F);
            ImGui::InputText("搜索##skill-search", search, searchSize);
            const auto visible = skill_editor::filter_skills(options, search, excludedIds);
            for (const auto& option : visible) {
                const auto label = skill_editor::skill_label(option);
                const bool isSelected = selected.has_value() && selected->id == option.id;
                if (ImGui::Selectable(label.c_str(), isSelected)) {
                    selected = option;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        return changed;
    }

    // --- GUI render helpers (static, called from the register_tab lambda) ---

    static void render_give_items(MyPalMod* self) {
        ImGui::TextUnformatted("Give items");
        ImGui::InputText("Item ID", self->item_buf_, sizeof(self->item_buf_));
        ImGui::InputInt("Count", &self->count_input_);
        self->count_input_ = clamp(self->count_input_, 1, 9999);
        if (ImGui::Button("Give")) {
            const std::lock_guard lock(self->req_mutex_);
            self->give_item_ = self->item_buf_;
            self->give_count_ = self->count_input_;
            self->give_requested_ = true;
        }
    }

    static void render_item_browser(MyPalMod* self) {
        if (ImGui::Button("Scan game items")) {
            self->want_scan_items_.store(true);
        }
        ImGui::SameLine();
        ImGui::InputText("##search", self->search_buf_, sizeof(self->search_buf_));
        {
            const std::lock_guard lock(self->inv_mutex_);
            ImGui::TextDisabled("(%d items)", static_cast<int>(self->item_db_cache_.size()));
        }
        ImGui::BeginChild("browser", ImVec2(380, 160), true);
        {
            std::string filter(self->search_buf_);
            for (auto& c : filter) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            auto tryItem = [&](const char* raw) {
                std::string lower(raw);
                for (auto& c : lower) {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                if (!filter.empty() && lower.find(filter) == std::string::npos) {
                    return;
                }
                if (ImGui::Selectable(raw)) {
                    const auto copyLen =
                        std::min<std::size_t>(std::strlen(raw), sizeof(self->item_buf_) - 1);
                    std::memcpy(self->item_buf_, raw, copyLen);
                    self->item_buf_[copyLen] = '\0';
                }
            };
            const std::lock_guard lock(self->inv_mutex_);
            if (self->item_db_cache_.empty()) {
                ImGui::TextDisabled("尚未发现物品，请重新扫描。");
            }
            for (const auto& item : self->item_db_cache_) {
                tryItem(item.c_str());
            }
        }
        ImGui::EndChild();
    }

    static void render_inventory(MyPalMod* self) {
        if (ImGui::Button("Refresh inventory")) {
            self->want_read_.store(true);
        }
        ImGui::SameLine();
        ImGui::TextUnformatted("(click an item to select, then set count)");
        {
            const std::lock_guard lock(self->inv_mutex_);
            ImGui::BeginChild("invlist", ImVec2(380, 220), true);
            for (int i = 0; i < static_cast<int>(self->inv_cache_.size()); ++i) {
                const auto& e = self->inv_cache_[i];
                const std::string label =
                    e.item_id + "  x" + std::to_string(e.count) + " ##inv" + std::to_string(i);
                if (ImGui::Selectable(label.c_str(), self->selected_ == i)) {
                    self->selected_ = i;
                    self->set_count_input_ = e.count;
                }
            }
            ImGui::EndChild();

            if (self->selected_ >= 0 &&
                self->selected_ < static_cast<int>(self->inv_cache_.size())) {
                const auto& e = self->inv_cache_[self->selected_];
                ImGui::Text("Selected: %s (slot %d, x%d)", e.item_id.c_str(),
                            static_cast<int>(e.slot_index), e.count);
                ImGui::InputInt("New count", &self->set_count_input_);
                self->set_count_input_ = clamp(self->set_count_input_, 0, 9999);
                if (ImGui::Button("Set count")) {
                    const std::lock_guard lock2(self->req_mutex_);
                    self->modify_slot_ = e.slot_index;
                    self->modify_count_ = self->set_count_input_;
                    self->modify_requested_ = true;
                }
            }
        }
    }

    static void render_passive_skills(MyPalMod* self, const SkillEditorSnapshot& snapshot,
                                      const bool pending) {
        ImGui::Text("被动技能 (%d/4)", static_cast<int>(snapshot.state.passiveIds.size()));
        std::unordered_set<std::string> excluded(snapshot.state.passiveIds.begin(),
                                                 snapshot.state.passiveIds.end());

        ImGui::BeginDisabled(pending);
        for (std::size_t index = 0; index < snapshot.state.passiveIds.size(); ++index) {
            const auto& id = snapshot.state.passiveIds[index];
            const auto label = find_skill_label(snapshot.catalog.passiveSkills, id);
            ImGui::Text("%d. %s", static_cast<int>(index + 1), label.c_str());
            ImGui::SameLine();
            const auto replaceId = "替换##passive-" + std::to_string(index);
            if (ImGui::Button(replaceId.c_str())) {
                self->passiveEditIndex_ = static_cast<int>(index);
                self->passiveChoice_.reset();
                self->passiveSearch_[0] = '\0';
            }
            ImGui::SameLine();
            const auto removeId = "删除##passive-" + std::to_string(index);
            if (ImGui::Button(removeId.c_str())) {
                self->skillQueue_.push({.target = snapshot.target,
                                        .kind = skill_editor::SkillKind::passive,
                                        .operation = skill_editor::SkillEditOperation::remove,
                                        .oldPassiveId = id});
                self->passiveEditIndex_ = -1;
                self->passiveChoice_.reset();
            }
        }

        if (snapshot.state.passiveIds.empty()) {
            ImGui::TextDisabled("暂无被动技能");
        }
        if (snapshot.state.passiveIds.size() < 4 && ImGui::Button("新增被动技能")) {
            self->passiveEditIndex_ = -2;
            self->passiveChoice_.reset();
            self->passiveSearch_[0] = '\0';
        }
        ImGui::EndDisabled();

        if (self->passiveEditIndex_ == -1) {
            return;
        }

        const bool replacing = self->passiveEditIndex_ >= 0;
        ImGui::TextUnformatted(replacing ? "选择替换后的被动技能：" : "选择要新增的被动技能：");
        ImGui::BeginDisabled(pending || !snapshot.catalog.ready);
        render_skill_picker("##passive-picker", snapshot.catalog.passiveSkills, excluded,
                            self->passiveSearch_, sizeof(self->passiveSearch_),
                            self->passiveChoice_);
        const bool canConfirm =
            self->passiveChoice_.has_value() &&
            (!replacing ||
             self->passiveEditIndex_ < static_cast<int>(snapshot.state.passiveIds.size()));
        ImGui::BeginDisabled(!canConfirm);
        if (ImGui::Button("确认被动技能修改")) {
            skill_editor::SkillEditRequest request{
                .target = snapshot.target,
                .kind = skill_editor::SkillKind::passive,
                .operation = replacing ? skill_editor::SkillEditOperation::replace
                                       : skill_editor::SkillEditOperation::add,
                .newPassiveId = self->passiveChoice_->id,
            };
            if (replacing) {
                request.oldPassiveId =
                    snapshot.state.passiveIds[static_cast<std::size_t>(self->passiveEditIndex_)];
            }
            self->skillQueue_.push(std::move(request));
            self->passiveEditIndex_ = -1;
            self->passiveChoice_.reset();
        }
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("取消##passive")) {
            self->passiveEditIndex_ = -1;
            self->passiveChoice_.reset();
        }
    }

    static void render_active_skills(MyPalMod* self, const SkillEditorSnapshot& snapshot,
                                     const bool pending) {
        ImGui::TextUnformatted("主动技能（EquipWaza）");
        std::unordered_set<std::string> excluded;
        for (const auto& skill : snapshot.state.activeSkills) {
            excluded.insert(skill.id);
        }

        ImGui::BeginDisabled(pending);
        for (std::size_t slot = 0; slot < 3; ++slot) {
            if (slot < snapshot.state.activeSkills.size()) {
                const auto& skill = snapshot.state.activeSkills[slot];
                const auto label = find_skill_label(snapshot.catalog.activeSkills, skill.id);
                ImGui::Text("槽位 %d：%s", static_cast<int>(slot + 1), label.c_str());
                ImGui::SameLine();
                const auto replaceId = "替换##active-" + std::to_string(slot);
                if (ImGui::Button(replaceId.c_str())) {
                    self->activeEditSlot_ = static_cast<int>(slot);
                    self->activeChoice_.reset();
                    self->activeSearch_[0] = '\0';
                }
                ImGui::SameLine();
                const auto clearId = "清空##active-" + std::to_string(slot);
                if (ImGui::Button(clearId.c_str())) {
                    self->skillQueue_.push({.target = snapshot.target,
                                            .kind = skill_editor::SkillKind::active,
                                            .operation = skill_editor::SkillEditOperation::remove,
                                            .activeSlot = slot});
                    self->activeEditSlot_ = -1;
                    self->activeChoice_.reset();
                }
            } else {
                ImGui::Text("槽位 %d：空", static_cast<int>(slot + 1));
                if (slot == snapshot.state.activeSkills.size()) {
                    ImGui::SameLine();
                    const auto equipId = "选择/装备##active-" + std::to_string(slot);
                    if (ImGui::Button(equipId.c_str())) {
                        self->activeEditSlot_ = static_cast<int>(slot);
                        self->activeChoice_.reset();
                        self->activeSearch_[0] = '\0';
                    }
                }
            }
        }
        ImGui::EndDisabled();

        if (self->activeEditSlot_ < 0) {
            return;
        }

        const auto slot = static_cast<std::size_t>(self->activeEditSlot_);
        const bool replacing = slot < snapshot.state.activeSkills.size();
        ImGui::Text("为槽位 %d 选择主动技能：", self->activeEditSlot_ + 1);
        ImGui::BeginDisabled(pending || !snapshot.catalog.ready);
        render_skill_picker("##active-picker", snapshot.catalog.activeSkills, excluded,
                            self->activeSearch_, sizeof(self->activeSearch_), self->activeChoice_);
        const bool canConfirm =
            self->activeChoice_.has_value() && self->activeChoice_->activeValue.has_value();
        ImGui::BeginDisabled(!canConfirm);
        if (ImGui::Button("确认主动技能修改")) {
            self->skillQueue_.push(
                {.target = snapshot.target,
                 .kind = skill_editor::SkillKind::active,
                 .operation = replacing ? skill_editor::SkillEditOperation::replace
                                        : skill_editor::SkillEditOperation::add,
                 .activeSlot = slot,
                 .newActiveSkill = skill_editor::ActiveSkill{
                     .value = *self->activeChoice_->activeValue, .id = self->activeChoice_->id}});
            self->activeEditSlot_ = -1;
            self->activeChoice_.reset();
        }
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("取消##active")) {
            self->activeEditSlot_ = -1;
            self->activeChoice_.reset();
        }
    }

    static void render_pal_editor(MyPalMod* self) {
        if (!ImGui::CollapsingHeader("Pal editor")) {
            return;
        }

        SkillEditorSnapshot snapshot;
        {
            const std::lock_guard lock(self->skillSnapshotMutex_);
            snapshot = self->skillSnapshot_;
        }
        if (self->skillUiTarget_ != snapshot.target) {
            self->skillUiTarget_ = snapshot.target;
            reset_skill_editor_ui(self);
        }

        if (snapshot.target != 0) {
            const char* source =
                snapshot.source == SkillTargetSource::viewed ? "当前查看" : "手动选择";
            ImGui::TextColored(ImVec4(0.4F, 1.0F, 0.4F, 1.0F), "目标：%s（%s）",
                               snapshot.palName.empty() ? "(读取中...)" : snapshot.palName.c_str(),
                               source);
        } else {
            ImGui::TextDisabled("请在游戏中查看一只帕鲁，或从下方扫描列表中手动选择。");
        }
        const bool pending = snapshot.pending || self->skillQueue_.size() != 0;
        ImGui::SameLine();
        ImGui::BeginDisabled(pending || snapshot.target == 0);
        if (ImGui::Button("刷新技能列表")) {
            self->wantRefreshSkillCatalog_.store(true);
        }
        ImGui::EndDisabled();
        if (pending) {
            ImGui::TextColored(ImVec4(1.0F, 0.8F, 0.2F, 1.0F), "技能修改处理中...");
        }
        if (!snapshot.lastResult.empty()) {
            ImGui::TextWrapped("结果：%s", snapshot.lastResult.c_str());
        }
        if (!snapshot.catalog.error.empty()) {
            ImGui::TextColored(ImVec4(1.0F, 0.45F, 0.35F, 1.0F), "技能目录：%s",
                               snapshot.catalog.error.c_str());
        }
        ImGui::Separator();

        // Pal list (manual selection fallback)
        if (ImGui::Button("Scan Pals")) {
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
            for (int i = 0; i < static_cast<int>(self->pal_cache_.size()); ++i) {
                const std::string palLabel =
                    self->pal_cache_[i].name + " ##pal" + std::to_string(i);
                if (ImGui::Selectable(palLabel.c_str(), self->pal_selected_ == i)) {
                    self->pal_selected_ = i;
                }
            }
        }
        ImGui::EndChild();

        if (snapshot.target == 0) {
            return;
        }

        ImGui::Separator();
        render_passive_skills(self, snapshot, pending);
        ImGui::Separator();
        render_active_skills(self, snapshot, pending);
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
    std::atomic<bool> suppressViewTracking_{false};
    UFunction* fnGetPSL_{};
    std::string viewedPalName_;
    uintptr_t lastCachedPal_{0};

    pal_skills::PalSkillGateway skillGateway_;
    skill_editor::SkillEditQueue skillQueue_;
    std::mutex skillSnapshotMutex_;
    SkillEditorSnapshot skillSnapshot_;
    std::atomic<bool> wantRefreshSkillCatalog_{true};
    skill_editor::SkillTarget lastSkillTarget_{};

    // Skill editor UI state (GUI thread only)
    char passiveSearch_[96]{};
    char activeSearch_[96]{};
    int passiveEditIndex_ = -1;  // -2 = add, >= 0 = replace index
    int activeEditSlot_ = -1;
    std::optional<skill_editor::SkillOption> passiveChoice_;
    std::optional<skill_editor::SkillOption> activeChoice_;
    skill_editor::SkillTarget skillUiTarget_{};
};

#define MYPALMOD_API __declspec(dllexport)
extern "C" {
MYPALMOD_API CppUserModBase* start_mod() {
    return new MyPalMod();
}

MYPALMOD_API void uninstall_mod(CppUserModBase* mod) {
    delete mod;
}
}
