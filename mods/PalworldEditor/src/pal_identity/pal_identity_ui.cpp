/**
 * @file pal_identity_ui.cpp
 * @brief Alpha、Lucky 与觉醒三个独立形态开关的 ImGui 渲染实现。
 * @details 只在 GUI 线程调用；控件不直接访问 Unreal 对象。实现从 src/mod/dllmain.cpp
 *          拆出，签名不变。
 */
#include <imgui.h>
#include <mod/editor_ui.hpp>
#include <mod/mod_core.hpp>

void PalworldEditorMod::render_pal_identity(PalworldEditorMod* self,
                                            const SkillEditorSnapshot& snapshot,
                                            const bool mutationsDisabled) {
    editor_ui::section_header("形态修改");
    const auto& identity = snapshot.palIdentity;
    if (!identity.readable) {
        ImGui::TextDisabled("形态字段读取中或当前游戏版本不支持安全修改");
        return;
    }

    self->identityDraft_.reconcile(identity, snapshot.targetGeneration);
    auto& values = self->identityDraft_.values();
    ImGui::Text("当前 CharacterID：%s", identity.characterId.c_str());

    ImGui::BeginDisabled(mutationsDisabled || identity.summoned || !identity.alphaAvailable);
    ImGui::Checkbox("头目 / Alpha##pal-alpha", &values.alpha);
    ImGui::EndDisabled();
    if (!identity.alphaAvailable) {
        ImGui::SameLine();
        ImGui::TextDisabled("（未找到经原生数据库验证的普通/BOSS 配对）");
    }

    ImGui::BeginDisabled(mutationsDisabled || identity.summoned);
    ImGui::Checkbox("闪光 / Lucky##pal-lucky", &values.lucky);
    ImGui::SameLine();
    ImGui::Checkbox("觉醒##pal-awakening", &values.awakening);
    ImGui::EndDisabled();

    if (identity.summoned) {
        ImGui::TextColored(ImVec4(1.0F, 0.8F, 0.2F, 1.0F),
                           "当前帕鲁正在场上；请先按 E 收回，再应用形态修改。");
    }
    ImGui::TextDisabled("三个维度相互独立；不消耗材料。Alpha 会切换普通/BOSS CharacterID。");

    const auto request = self->identityDraft_.make_request(snapshot.worldGeneration);
    ImGui::BeginDisabled(mutationsDisabled || identity.summoned || !request.has_value());
    {
        editor_ui::scoped_accent_button accent;
        if (ImGui::Button("应用形态修改")) {
            self->identityRequestSlot_.submit(*request);
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("（写入、原生刷新、重读；失败自动回滚）");
}
