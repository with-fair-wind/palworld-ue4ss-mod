/**
 * @file stat_ui.cpp
 * @brief 持久化个体属性编辑区的 ImGui 渲染实现。
 * @details 只在 GUI 线程调用；点击应用后只提交相对快照发生变化的字段。实现从
 *          src/mod/dllmain.cpp 拆出，签名不变。
 */
#include <array>
#include <string>

#include <imgui.h>
#include <mod/editor_ui.hpp>
#include <mod/mod_core.hpp>

void PalworldEditorMod::render_pal_stats(PalworldEditorMod* self,
                                         const SkillEditorSnapshot& snapshot,
                                         const bool mutationsDisabled,
                                         const bool workSuitabilityMutationsDisabled) {
    editor_ui::section_header("属性修改");
    const auto& stats = snapshot.palStat;
    if (!stats.readable) {
        ImGui::TextDisabled("属性读取中或不可用");
        return;
    }
    self->statDraft_.reconcile(stats, snapshot.targetGeneration);
    auto& values = self->statDraft_.values();
    ImGui::Text("当前：等级 %d / HP %d / 攻击 %d / 防御 %d / 亲密度 %d", stats.level,
                stats.talentHp, stats.talentShot, stats.talentDefense, stats.friendshipRank);
    ImGui::Text("强化：最大HP %d / 攻击 %d / 防御 %d / 工作速度 %d", stats.soulHpRank,
                stats.soulAttackRank, stats.soulDefenseRank, stats.soulWorkSpeedRank);
    const char* const currentGender = stats.gender == pal_stats::PalGender::male     ? "雄性"
                                      : stats.gender == pal_stats::PalGender::female ? "雌性"
                                                                                     : "未设置";
    ImGui::Text("浓缩：%d 星 / 伙伴技能 Lv.%d / 性别：%s", stats.condensationStars,
                stats.partnerSkillLevel, currentGender);

    ImGui::SetNextItemWidth(120.0F);
    ImGui::DragInt("等级##stat-level", &values.level, 1.0F, pal_stats::kLevelMin,
                   pal_stats::kLevelMax);
    ImGui::SetNextItemWidth(120.0F);
    ImGui::DragInt("个体值·HP##stat-hp", &values.talentHp, 1.0F, pal_stats::kTalentMin,
                   pal_stats::kTalentMax);
    ImGui::SetNextItemWidth(120.0F);
    ImGui::DragInt("个体值·攻击##stat-atk", &values.talentShot, 1.0F, pal_stats::kTalentMin,
                   pal_stats::kTalentMax);
    ImGui::SetNextItemWidth(120.0F);
    ImGui::DragInt("个体值·防御##stat-def", &values.talentDefense, 1.0F, pal_stats::kTalentMin,
                   pal_stats::kTalentMax);

    ImGui::TextUnformatted("帕鲁之魂强化（直接编辑，不消耗材料）");
    ImGui::SetNextItemWidth(120.0F);
    ImGui::DragInt("强化·最大HP##stat-soul-hp", &values.soulHpRank, 1.0F, pal_stats::kSoulRankMin,
                   pal_stats::kSoulRankMax);
    ImGui::SetNextItemWidth(120.0F);
    ImGui::DragInt("强化·攻击##stat-soul-atk", &values.soulAttackRank, 1.0F,
                   pal_stats::kSoulRankMin, pal_stats::kSoulRankMax);
    ImGui::SetNextItemWidth(120.0F);
    ImGui::DragInt("强化·防御##stat-soul-def", &values.soulDefenseRank, 1.0F,
                   pal_stats::kSoulRankMin, pal_stats::kSoulRankMax);
    ImGui::SetNextItemWidth(120.0F);
    ImGui::DragInt("强化·工作速度##stat-soul-work", &values.soulWorkSpeedRank, 1.0F,
                   pal_stats::kSoulRankMin, pal_stats::kSoulRankMax);

    ImGui::TextUnformatted("浓缩与性别（直接编辑，不消耗材料）");
    ImGui::SetNextItemWidth(120.0F);
    ImGui::DragInt("浓缩星级##stat-condensation", &values.condensationStars, 1.0F,
                   pal_stats::kCondensationStarsMin, stats.condensationMaxStars);
    const char* const genderPreview = values.gender == pal_stats::PalGender::male     ? "雄性"
                                      : values.gender == pal_stats::PalGender::female ? "雌性"
                                                                                      : "未设置";
    ImGui::SetNextItemWidth(120.0F);
    if (ImGui::BeginCombo("性别##stat-gender", genderPreview)) {
        if (ImGui::Selectable("雄性", values.gender == pal_stats::PalGender::male)) {
            values.gender = pal_stats::PalGender::male;
        }
        if (ImGui::Selectable("雌性", values.gender == pal_stats::PalGender::female)) {
            values.gender = pal_stats::PalGender::female;
        }
        ImGui::EndCombo();
    }

    static constexpr std::array<const char*, pal_stats::kWorkSuitabilityCount>
        workSuitabilityLabels{
            "生火", "浇水", "播种", "发电", "手工作业", "采集", "伐木",
            "采矿", "采油", "制药", "冷却", "搬运",     "牧场",
        };
    if (ImGui::TreeNode("工作适应性永久附加值##stat-work-suitability")) {
        ImGui::TextDisabled(
            "直接编辑存档永久附加值；原生接口按当前值差量提交，不会创建物种没有的适应性。");
        for (std::size_t index{}; index < workSuitabilityLabels.size(); ++index) {
            const int maxBonus = pal_stats::max_editable_work_suitability_bonus(
                stats.workSuitabilityBaseRanks[index], stats.workSuitabilityBonusRanks[index],
                stats.workSuitabilityMaxRank);
            const bool supported = stats.workSuitabilityBaseRanks[index] > 0 ||
                                   stats.workSuitabilityBonusRanks[index] > 0;
            ImGui::BeginDisabled(workSuitabilityMutationsDisabled || !supported);
            ImGui::SetNextItemWidth(120.0F);
            const std::string label =
                std::string{workSuitabilityLabels[index]} + "##stat-work-" + std::to_string(index);
            ImGui::DragInt(label.c_str(), &values.workSuitabilityBonusRanks[index], 1.0F, 0,
                           maxBonus, "%d", ImGuiSliderFlags_ClampZeroRange);
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (supported) {
                ImGui::TextDisabled(
                    "(当前 Lv.%d = 固有 %d + 永久附加 +%d)", stats.workSuitabilityBaseRanks[index],
                    stats.workSuitabilityBaseRanks[index] - stats.workSuitabilityBonusRanks[index],
                    stats.workSuitabilityBonusRanks[index]);
            } else {
                ImGui::TextDisabled("(该物种不具备此适应性)");
            }
        }
        const auto workRequest =
            self->statDraft_.make_work_suitability_request(snapshot.worldGeneration);
        ImGui::BeginDisabled(workSuitabilityMutationsDisabled || !workRequest.has_value());
        {
            editor_ui::scoped_accent_button accent;
            if (ImGui::Button("应用工作适应性修改")) {
                self->statRequestSlot_.submit(*workRequest);
            }
        }
        ImGui::EndDisabled();
        ImGui::TreePop();
    }

    ImGui::SetNextItemWidth(120.0F);
    ImGui::DragInt("亲密度##stat-friend", &values.friendshipRank, 1.0F,
                   pal_stats::kFriendshipRankMin, pal_stats::kFriendshipRankMax);

    const auto coreRequest = self->statDraft_.make_core_request(snapshot.worldGeneration);
    ImGui::BeginDisabled(mutationsDisabled || !coreRequest.has_value());
    {
        editor_ui::scoped_accent_button accent;
        if (ImGui::Button("应用基础属性修改")) {
            self->statRequestSlot_.submit(*coreRequest);
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("(写入存档；重新召唤或重载后面板刷新)");
}
