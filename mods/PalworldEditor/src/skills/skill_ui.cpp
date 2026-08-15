/**
 * @file skill_ui.cpp
 * @brief 技能目录状态、被动/主动技能编辑、被动分类选择器与帕鲁编辑面板的 ImGui 渲染实现。
 * @details 只在 GUI 线程调用；本文件还包含仅供这些渲染函数使用的静态查找与类别辅助。
 *          实现从 src/mod/dllmain.cpp 拆出，签名不变。
 */
#include <algorithm>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include <imgui.h>
#include <mod/editor_ui.hpp>
#include <mod/mod_core.hpp>
#include <skills/passive_skill_presets.hpp>

auto PalworldEditorMod::find_skill_label(const std::vector<skill_editor::SkillOption>& options,
                                         const std::string_view id) -> std::string {
    const auto found = std::ranges::find(options, id, &skill_editor::SkillOption::id);
    return found == options.end() ? std::string(id) : skill_editor::skill_label(*found);
}

auto PalworldEditorMod::find_skill_option(const std::vector<skill_editor::SkillOption>& options,
                                          const std::string_view id)
    -> const skill_editor::SkillOption* {
    const auto found = std::ranges::find(options, id, &skill_editor::SkillOption::id);
    return found == options.end() ? nullptr : &*found;
}

void PalworldEditorMod::reset_skill_editor_ui(PalworldEditorMod* self) {
    self->passivePresetIndex_.reset();
    self->passiveEditIndex_ = -1;
    self->activeEditSlot_ = -1;
    self->passivePickerState_.reset();
    self->activeChoice_.reset();
    self->passiveSearch_[0] = '\0';
    self->activeSearch_[0] = '\0';
    self->statDraft_.reset();
    self->identityDraft_.reset();
}

auto PalworldEditorMod::passive_category_label(
    const std::optional<skill_editor::PassiveSkillCategory> category) -> const char* {
    if (!category.has_value()) {
        return "全部";
    }
    switch (*category) {
        case skill_editor::PassiveSkillCategory::normal:
            return "普通";
        case skill_editor::PassiveSkillCategory::rare:
            return "稀有";
        case skill_editor::PassiveSkillCategory::premium:
            return "极品";
        case skill_editor::PassiveSkillCategory::legendary:
            return "传说";
        case skill_editor::PassiveSkillCategory::negative:
            return "负面";
    }
    return "全部";
}

auto PalworldEditorMod::passive_category_color(const skill_editor::PassiveSkillCategory category)
    -> ImVec4 {
    switch (category) {
        case skill_editor::PassiveSkillCategory::normal:
            return {0.92F, 0.92F, 0.92F, 1.0F};
        case skill_editor::PassiveSkillCategory::rare:
            return {1.0F, 0.82F, 0.20F, 1.0F};
        case skill_editor::PassiveSkillCategory::premium:
            return {0.30F, 0.65F, 1.0F, 1.0F};
        case skill_editor::PassiveSkillCategory::legendary:
            return {0.72F, 0.40F, 1.0F, 1.0F};
        case skill_editor::PassiveSkillCategory::negative:
            return {1.0F, 0.30F, 0.30F, 1.0F};
    }
    return {1.0F, 1.0F, 1.0F, 1.0F};
}

auto PalworldEditorMod::render_skill_picker(
    const char* id, const std::vector<skill_editor::SkillOption>& options,
    const std::optional<skill_editor::ActiveSkillCategory> category,
    const std::unordered_set<std::string>& excludedIds, char* search, const std::size_t searchSize,
    std::optional<skill_editor::SkillOption>& selected) -> bool {
    const std::string preview =
        selected.has_value() ? skill_editor::skill_label(*selected) : "请选择技能";
    bool changed = false;
    if (ImGui::BeginCombo(id, preview.c_str())) {
        ImGui::SetNextItemWidth(340.0F);
        ImGui::InputText("搜索##skill-search", search, searchSize);
        const auto visible =
            skill_editor::filter_active_skill_views(options, category, search, excludedIds);
        for (const auto* const option : visible) {
            const auto label = skill_editor::skill_label(*option);
            const bool isSelected = selected.has_value() && selected->id == option->id;
            if (ImGui::Selectable(label.c_str(), isSelected)) {
                selected = *option;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

void PalworldEditorMod::render_passive_category_picker(
    PalworldEditorMod* self, const skill_editor::SkillCatalogSnapshot& catalog) {
    const auto& classification = catalog.passiveClassification;
    const bool ready = classification.ready;
    const auto completed = self->passiveClassificationCompleted_.load(std::memory_order_relaxed);
    const auto total = self->passiveClassificationTotal_.load(std::memory_order_relaxed);

    ImGui::SetNextItemWidth(160.0F);
    if (ImGui::BeginCombo("类别##passive-category",
                          passive_category_label(self->passivePickerState_.category))) {
        if (ImGui::Selectable("全部", !self->passivePickerState_.category.has_value())) {
            (void)self->passivePickerState_.set_category(std::nullopt);
        }
        const skill_editor::PassiveSkillCategory concreteCategories[] = {
            skill_editor::PassiveSkillCategory::normal,
            skill_editor::PassiveSkillCategory::rare,
            skill_editor::PassiveSkillCategory::premium,
            skill_editor::PassiveSkillCategory::legendary,
            skill_editor::PassiveSkillCategory::negative,
        };
        ImGui::BeginDisabled(!ready);
        for (const auto category : concreteCategories) {
            const bool isCurrent = self->passivePickerState_.category == category;
            ImGui::PushStyleColor(ImGuiCol_Text, passive_category_color(category));
            if (ImGui::Selectable(passive_category_label(category), isCurrent)) {
                (void)self->passivePickerState_.set_category(category);
            }
            ImGui::PopStyleColor();
        }
        ImGui::EndDisabled();
        ImGui::EndCombo();
    }

    if (!ready && classification.error.empty()) {
        ImGui::TextDisabled("正在读取被动技能分类：%d/%d", static_cast<int>(completed),
                            static_cast<int>(total));
    } else if (!classification.error.empty()) {
        if (ready) {
            ImGui::TextColored(passive_category_color(skill_editor::PassiveSkillCategory::rare),
                               "正在使用上一次成功分类（本次读取失败：%s）",
                               classification.error.c_str());
        } else {
            ImGui::TextColored(passive_category_color(skill_editor::PassiveSkillCategory::negative),
                               "被动技能分类失败：%s（仅“全部”可用）",
                               classification.error.c_str());
        }
    }
}

auto PalworldEditorMod::render_passive_skill_picker(
    PalworldEditorMod* self, const std::vector<skill_editor::SkillOption>& options,
    const std::unordered_set<std::string>& excludedIds) -> bool {
    const auto& selected = self->passivePickerState_.selected;
    const std::string preview =
        selected.has_value() ? skill_editor::skill_label(*selected) : "请选择技能";
    const bool coloredPreview = selected.has_value() && selected->passiveMetadata.has_value();
    if (coloredPreview) {
        ImGui::PushStyleColor(ImGuiCol_Text,
                              passive_category_color(selected->passiveMetadata->category));
    }
    bool changed = false;
    if (ImGui::BeginCombo("##passive-skill-picker", preview.c_str())) {
        ImGui::SetNextItemWidth(340.0F);
        ImGui::InputText("搜索##passive-skill-search", self->passiveSearch_,
                         sizeof(self->passiveSearch_));
        const auto visible = skill_editor::filter_passive_skill_views(
            options, self->passivePickerState_.category, self->passiveSearch_, excludedIds);
        for (const auto* option : visible) {
            const auto label = skill_editor::skill_label(*option);
            const bool isSelected = selected.has_value() && selected->id == option->id;
            if (option->passiveMetadata.has_value()) {
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      passive_category_color(option->passiveMetadata->category));
            }
            if (ImGui::Selectable(label.c_str(), isSelected)) {
                self->passivePickerState_.selected = *option;
                changed = true;
            }
            if (option->passiveMetadata.has_value()) {
                ImGui::PopStyleColor();
            }
        }
        ImGui::EndCombo();
    }
    if (coloredPreview) {
        ImGui::PopStyleColor();
    }
    return changed;
}

void PalworldEditorMod::render_passive_skills(PalworldEditorMod* self,
                                              const SkillEditorSnapshot& snapshot,
                                              const bool mutationsDisabled) {
    const auto presets = skill_editor::passive_skill_presets();
    const bool presetSelectionValid =
        self->passivePresetIndex_.has_value() && *self->passivePresetIndex_ < presets.size();
    const auto presetPreview = presetSelectionValid
                                   ? presets[*self->passivePresetIndex_].displayName
                                   : std::string_view{"请选择词条预设"};

    editor_ui::section_header("词条预设");
    ImGui::BeginDisabled(mutationsDisabled);
    if (ImGui::BeginCombo("词条预设##passive-preset", presetPreview.data())) {
        for (std::size_t index{}; index < presets.size(); ++index) {
            const bool selected =
                self->passivePresetIndex_.has_value() && *self->passivePresetIndex_ == index;
            if (ImGui::Selectable(presets[index].displayName.data(), selected)) {
                self->passivePresetIndex_ = index;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    if (presetSelectionValid) {
        const auto& preset = presets[*self->passivePresetIndex_];
        for (const auto id : preset.passiveIds) {
            const auto label = find_skill_label(snapshot.catalog.passive.skills, id);
            ImGui::BulletText("%s", label.c_str());
        }
    }

    ImGui::BeginDisabled(!presetSelectionValid);
    {
        editor_ui::scoped_accent_button accent;
        if (ImGui::Button("应用预设")) {
            self->skillQueue_.push(skill_editor::make_passive_preset_request(
                presets[*self->passivePresetIndex_], snapshot.targetGeneration,
                snapshot.worldGeneration));
            self->passiveEditIndex_ = -1;
            self->passivePickerState_.clear_selection();
        }
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    ImGui::Separator();

    ImGui::Text("被动技能 (%d/4)", static_cast<int>(snapshot.state.passiveIds.size()));
    std::unordered_set<std::string> excluded(snapshot.state.passiveIds.begin(),
                                             snapshot.state.passiveIds.end());

    ImGui::BeginDisabled(mutationsDisabled);
    for (std::size_t index = 0; index < snapshot.state.passiveIds.size(); ++index) {
        const auto& id = snapshot.state.passiveIds[index];
        const auto* option = find_skill_option(snapshot.catalog.passive.skills, id);
        const auto label = option != nullptr ? skill_editor::skill_label(*option) : std::string(id);
        if (option != nullptr && option->passiveMetadata.has_value()) {
            ImGui::TextColored(passive_category_color(option->passiveMetadata->category), "%d. %s",
                               static_cast<int>(index + 1), label.c_str());
        } else {
            ImGui::Text("%d. %s", static_cast<int>(index + 1), label.c_str());
        }
        ImGui::SameLine();
        const auto replaceId = "替换##passive-" + std::to_string(index);
        if (ImGui::Button(replaceId.c_str())) {
            self->passiveEditIndex_ = static_cast<int>(index);
            self->passivePickerState_.clear_selection();
        }
        ImGui::SameLine();
        const auto removeId = "删除##passive-" + std::to_string(index);
        if (ImGui::Button(removeId.c_str())) {
            self->skillQueue_.push({.targetGeneration = snapshot.targetGeneration,
                                    .worldGeneration = snapshot.worldGeneration,
                                    .kind = skill_editor::SkillKind::passive,
                                    .operation = skill_editor::SkillEditOperation::remove,
                                    .oldPassiveId = id});
            self->passiveEditIndex_ = -1;
            self->passivePickerState_.clear_selection();
        }
    }

    if (snapshot.state.passiveIds.empty()) {
        ImGui::TextDisabled("暂无被动技能");
    }
    if (snapshot.state.passiveIds.size() < 4 && ImGui::Button("新增被动技能")) {
        self->passiveEditIndex_ = -2;
        self->passivePickerState_.clear_selection();
    }
    ImGui::EndDisabled();

    if (self->passiveEditIndex_ == -1) {
        return;
    }

    const bool replacing = self->passiveEditIndex_ >= 0;
    ImGui::TextUnformatted(replacing ? "选择替换后的被动技能：" : "选择要新增的被动技能：");
    ImGui::BeginDisabled(mutationsDisabled || !snapshot.catalog.passive.ready);
    render_passive_category_picker(self, snapshot.catalog);
    render_passive_skill_picker(self, snapshot.catalog.passive.skills, excluded);
    const bool canConfirm = self->passivePickerState_.selected.has_value() &&
                            (!replacing || self->passiveEditIndex_ <
                                               static_cast<int>(snapshot.state.passiveIds.size()));
    ImGui::BeginDisabled(!canConfirm);
    {
        editor_ui::scoped_accent_button accent;
        if (ImGui::Button("确认被动技能修改")) {
            skill_editor::SkillEditRequest request{
                .targetGeneration = snapshot.targetGeneration,
                .worldGeneration = snapshot.worldGeneration,
                .kind = skill_editor::SkillKind::passive,
                .operation = replacing ? skill_editor::SkillEditOperation::replace
                                       : skill_editor::SkillEditOperation::add,
                .newPassiveId = self->passivePickerState_.selected->id,
            };
            if (replacing) {
                request.oldPassiveId =
                    snapshot.state.passiveIds[static_cast<std::size_t>(self->passiveEditIndex_)];
            }
            self->skillQueue_.push(std::move(request));
            self->passiveEditIndex_ = -1;
            self->passivePickerState_.clear_selection();
        }
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("取消##passive")) {
        self->passiveEditIndex_ = -1;
        self->passivePickerState_.clear_selection();
    }
}

void PalworldEditorMod::render_active_skills(PalworldEditorMod* self,
                                             const SkillEditorSnapshot& snapshot,
                                             const bool mutationsDisabled) {
    editor_ui::section_header("主动技能（EquipWaza）");
    std::unordered_set<std::string> excluded;
    for (const auto& skill : snapshot.state.activeSkills) {
        excluded.insert(skill.id);
    }

    ImGui::BeginDisabled(mutationsDisabled);
    for (std::size_t slot = 0; slot < 3; ++slot) {
        if (slot < snapshot.state.activeSkills.size()) {
            const auto& skill = snapshot.state.activeSkills[slot];
            const auto label = find_skill_label(snapshot.catalog.active.skills, skill.id);
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
                self->skillQueue_.push({.targetGeneration = snapshot.targetGeneration,
                                        .worldGeneration = snapshot.worldGeneration,
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
    ImGui::BeginDisabled(mutationsDisabled || !snapshot.catalog.active.ready);
    if (ImGui::BeginCombo("类别##active-category",
                          self->activeCategoryFilter_.has_value() ? "已选" : "全部")) {
        if (ImGui::Selectable("全部", !self->activeCategoryFilter_.has_value())) {
            self->activeCategoryFilter_.reset();
        }
        for (const auto cat :
             {skill_editor::ActiveSkillCategory::Melee, skill_editor::ActiveSkillCategory::Shot,
              skill_editor::ActiveSkillCategory::Support}) {
            const bool isCurrent = self->activeCategoryFilter_ == cat;
            if (ImGui::Selectable(cat == skill_editor::ActiveSkillCategory::Melee  ? "近战"
                                  : cat == skill_editor::ActiveSkillCategory::Shot ? "射击"
                                                                                   : "辅助",
                                  isCurrent)) {
                self->activeCategoryFilter_ = cat;
            }
        }
        ImGui::EndCombo();
    }
    render_skill_picker("##active-picker", snapshot.catalog.active.skills,
                        self->activeCategoryFilter_, excluded, self->activeSearch_,
                        sizeof(self->activeSearch_), self->activeChoice_);
    const bool canConfirm =
        self->activeChoice_.has_value() && self->activeChoice_->activeValue.has_value();
    ImGui::BeginDisabled(!canConfirm);
    {
        editor_ui::scoped_accent_button accent;
        if (ImGui::Button("确认主动技能修改")) {
            self->skillQueue_.push(
                {.targetGeneration = snapshot.targetGeneration,
                 .worldGeneration = snapshot.worldGeneration,
                 .kind = skill_editor::SkillKind::active,
                 .operation = replacing ? skill_editor::SkillEditOperation::replace
                                        : skill_editor::SkillEditOperation::add,
                 .activeSlot = slot,
                 .newActiveSkill = skill_editor::ActiveSkill{
                     .value = *self->activeChoice_->activeValue, .id = self->activeChoice_->id}});
            self->activeEditSlot_ = -1;
            self->activeChoice_.reset();
        }
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("取消##active")) {
        self->activeEditSlot_ = -1;
        self->activeChoice_.reset();
    }
}

void PalworldEditorMod::render_pal_editor(PalworldEditorMod* self) {
    if (!ImGui::CollapsingHeader("Pal editor")) {
        return;
    }

    std::shared_ptr<const SkillEditorSnapshot> publishedSnapshot;
    {
        const std::lock_guard lock(self->skillSnapshotMutex_);
        publishedSnapshot = self->skillSnapshot_;
    }
    const auto& snapshot = *publishedSnapshot;
    if (self->skillUiGeneration_ != snapshot.targetGeneration ||
        self->skillUiWorldGeneration_ != snapshot.worldGeneration) {
        self->skillUiGeneration_ = snapshot.targetGeneration;
        self->skillUiWorldGeneration_ = snapshot.worldGeneration;
        reset_skill_editor_ui(self);
    }
    const auto choiceStillExists = [](const std::optional<skill_editor::SkillOption>& choice,
                                      const skill_editor::SkillCatalogSection& section) {
        return !choice.has_value() ||
               std::ranges::any_of(section.skills, [&choice](const auto& option) {
                   return option.id == choice->id;
               });
    };
    if (!choiceStillExists(self->passivePickerState_.selected, snapshot.catalog.passive)) {
        self->passivePickerState_.clear_selection();
    }
    if (!choiceStillExists(self->activeChoice_, snapshot.catalog.active)) {
        self->activeChoice_.reset();
    }

    bool selectionPending = false;
    {
        const std::lock_guard lock(self->selectionRequestMutex_);
        selectionPending = self->selectCurrentPalRequest_.has_value();
    }
    const bool pending = snapshot.pending || self->skillQueue_.size() != 0 ||
                         self->statRequestSlot_.has_pending() ||
                         self->identityRequestSlot_.has_pending() || selectionPending;
    const bool catalogReady = skill_editor::catalog_is_ready_for_editing(snapshot.catalog);
    const bool lifecycleReady = snapshot.worldAccessible && snapshot.worldLifecycleCallbacksReady;
    const bool editingReady = lifecycleReady && snapshot.targetMatchesCurrent && catalogReady;
    ImGui::BeginDisabled(pending || !lifecycleReady);
    if (ImGui::Button("选择当前帕鲁")) {
        const std::lock_guard lock(self->selectionRequestMutex_);
        self->selectCurrentPalRequest_ = skill_editor::WorldBoundRequest{
            .worldGeneration = snapshot.worldGeneration,
        };
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(pending || !snapshot.worldAccessible);
    if (ImGui::Button("刷新技能列表")) {
        self->wantRefreshSkillCatalog_.store(true);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(pending || !lifecycleReady);
    {
        editor_ui::scoped_accent_button accent;
        if (ImGui::Button("复活队伍帕鲁")) {
            self->wantReviveTeam_.store(true, std::memory_order_release);
        }
    }
    ImGui::EndDisabled();

    if (snapshot.targetSelected) {
        ImGui::TextColored(ImVec4(0.4F, 1.0F, 0.4F, 1.0F), "当前已选择帕鲁：%s",
                           snapshot.palName.empty() ? "(读取中...)" : snapshot.palName.c_str());
    } else {
        ImGui::TextDisabled(
            "请用数字键高亮队伍帕鲁，再点击“选择当前帕鲁”；目标应与下一次按 E 召唤一致。");
    }
    if (snapshot.targetConfirmedForWorld &&
        snapshot.resolutionStatus != skill_editor::SelectedTargetResolutionStatus::success) {
        const auto message = skill_editor::resolution_status_message(snapshot.resolutionStatus);
        ImGui::TextColored(ImVec4(1.0F, 0.45F, 0.35F, 1.0F), "解析状态：%.*s",
                           static_cast<int>(message.size()), message.data());
    }
    if (!catalogReady) {
        ImGui::TextColored(ImVec4(1.0F, 0.8F, 0.2F, 1.0F),
                           "技能目录正在等待游戏初始化，将每 2 秒自动重试；"
                           "就绪前仅可查看技能。");
    }
    if (snapshot.targetSelected && !snapshot.targetMatchesCurrent) {
        const char* message = "暂时无法确认当前高亮目标；已保留选择并暂停技能修改。";
        if (!snapshot.targetConfirmedForWorld) {
            message = "世界已切换；已保留原选择用于显示，请重新点击“选择当前帕鲁”后再修改。";
        } else if (snapshot.resolutionStatus ==
                   skill_editor::SelectedTargetResolutionStatus::success) {
            message = "当前数字键高亮帕鲁与已选择目标不同；点击“选择当前帕鲁”后才会切换。";
        }
        ImGui::TextColored(ImVec4(1.0F, 0.8F, 0.2F, 1.0F), "%s", message);
    }
    if (!snapshot.worldLifecycleCallbacksReady) {
        ImGui::TextColored(ImVec4(1.0F, 0.45F, 0.35F, 1.0F),
                           "UE4SS 世界切换回调不可用；为防止存档/切图崩溃，技能编辑已停用。");
    } else if (!snapshot.worldAccessible) {
        ImGui::TextColored(ImVec4(1.0F, 0.8F, 0.2F, 1.0F),
                           "正在切换世界；所有待处理修改均已取消。");
    }
    if (snapshot.statWritesDisabled) {
        ImGui::TextColored(ImVec4(1.0F, 0.35F, 0.2F, 1.0F),
                           "属性写入已在本世界安全停用；请退出并重新进入世界。");
    }
    if (snapshot.workSuitabilityWritesDisabled) {
        ImGui::TextColored(ImVec4(1.0F, 0.35F, 0.2F, 1.0F),
                           "工作适应性写入已在本世界单独安全停用；其他属性仍可修改。");
    }
    if (snapshot.identityWritesDisabled) {
        ImGui::TextColored(ImVec4(1.0F, 0.35F, 0.2F, 1.0F),
                           "形态写入已在本世界安全停用；请退出并重新进入世界。");
    }
    if (pending) {
        ImGui::TextColored(ImVec4(1.0F, 0.8F, 0.2F, 1.0F), "技能修改处理中...");
    }
    if (!snapshot.lastResult.empty()) {
        ImGui::TextWrapped("结果：%s", snapshot.lastResult.c_str());
    }
    if (!snapshot.catalog.passive.error.empty()) {
        ImGui::TextColored(ImVec4(1.0F, 0.45F, 0.35F, 1.0F), "被动技能目录：%s",
                           snapshot.catalog.passive.error.c_str());
    }
    if (!snapshot.catalog.active.error.empty()) {
        ImGui::TextColored(ImVec4(1.0F, 0.45F, 0.35F, 1.0F), "主动技能目录：%s",
                           snapshot.catalog.active.error.c_str());
    }
    ImGui::Separator();

    if (!snapshot.targetSelected) {
        return;
    }

    ImGui::Separator();
    render_passive_skills(self, snapshot, pending || !editingReady);
    ImGui::Separator();
    render_active_skills(self, snapshot, pending || !editingReady);
    ImGui::Separator();
    const bool targetEditingReady = lifecycleReady && snapshot.targetMatchesCurrent;
    render_pal_identity(self, snapshot,
                        pending || !targetEditingReady || snapshot.identityWritesDisabled);
    ImGui::Separator();
    render_pal_stats(self, snapshot, pending || !targetEditingReady || snapshot.statWritesDisabled,
                     pending || !targetEditingReady || snapshot.workSuitabilityWritesDisabled);
}
