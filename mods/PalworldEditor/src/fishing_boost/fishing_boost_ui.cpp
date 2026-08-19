/**
 * @file fishing_boost_ui.cpp
 * @brief 钓鱼圣手的 ImGui 开关渲染。
 */
#include <imgui.h>
#include <mod/mod_core.hpp>

void PalworldEditorMod::render_fishing_boost(PalworldEditorMod* self) {
    ImGui::SeparatorText("钓鱼圣手");
    bool enabled = self->requestedFishingBoost_.load(std::memory_order_acquire);
    const auto phase = self->fishingBoostPhase_.load(std::memory_order_acquire);
    if (phase == fishing_boost::Phase::safetyDisabled) {
        ImGui::TextColored(ImVec4(1.0F, 0.35F, 0.2F, 1.0F), "字段布局或恢复失败；已安全停用。");
        return;
    }
    if (ImGui::Checkbox("即时钓鱼（无小游戏）", &enabled)) {
        self->requestedFishingBoost_.store(enabled, std::memory_order_release);
        self->fishingBoostDirty_.store(true, std::memory_order_release);
    }
    if (enabled) {
        ImGui::Indent();
        for (const auto& field : fishing_boost::kFieldCatalog) {
            ImGui::TextDisabled("%s: %.1f", field.description.data(), field.overrideValue);
        }
        ImGui::Unindent();
    }
    ImGui::TextDisabled("仅本进程有效；关闭/切图自动恢复原值。");
}
