/**
 * @file game_settings_ui.cpp
 * @brief 游戏参数覆盖的 ImGui 列表渲染。
 */
#include <string>

#include <common/hotkey_capture_ui.hpp>
#include <imgui.h>
#include <mod/mod_core.hpp>

void PalworldEditorMod::render_game_settings(PalworldEditorMod* self) {
    ImGui::SeparatorText("游戏参数");
    if (!ImGui::CollapsingHeader("参数覆盖", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }

    const auto phase = self->gameSettingsPhase_.load(std::memory_order_acquire);
    if (phase == game_settings::RuntimePhase::safetyDisabled) {
        ImGui::TextColored(ImVec4(1.0F, 0.35F, 0.2F, 1.0F),
                           "字段布局或写入验证失败；已安全停用，切换开关不会绕过。");
        return;
    }

    // 按分类分组渲染
    game_settings::Category currentCategory{};
    bool headerOpen = false;
    for (std::size_t i{}; i < game_settings::kOverrideCount; ++i) {
        const auto& spec = game_settings::kOverrideCatalog[i];
        if (spec.category != currentCategory || !headerOpen) {
            if (headerOpen) {
                ImGui::TreePop();
            }
            currentCategory = spec.category;
            headerOpen = ImGui::TreeNode(game_settings::category_name(currentCategory).data());
            if (!headerOpen) {
                continue;
            }
        }

        const auto desired = self->gameSettingsLedger_.desired(i);
        bool enabled = desired.has_value();

        // 每个参数一行：复选框 + 值输入
        if (std::holds_alternative<std::int32_t>(spec.defaultValue)) {
            int value = enabled ? std::get<std::int32_t>(*desired)
                                : std::get<std::int32_t>(spec.defaultValue);
            if (ImGui::Checkbox(spec.displayName.data(), &enabled)) {
                if (enabled) {
                    self->gameSettingsLedger_.set_desired(i, spec.defaultValue);
                } else {
                    self->gameSettingsLedger_.clear_desired(i);
                }
                self->gameSettingsDirty_.store(true, std::memory_order_release);
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120.0F);
            if (ImGui::InputInt("##val", &value, 1, 10)) {
                const auto lo = std::get<std::int32_t>(spec.minValue);
                const auto hi = std::get<std::int32_t>(spec.maxValue);
                value = value < lo ? lo : (value > hi ? hi : value);
                self->gameSettingsLedger_.set_desired(i, value);
                self->gameSettingsDirty_.store(true, std::memory_order_release);
            }
        } else {
            bool checked = enabled;
            if (ImGui::Checkbox(spec.displayName.data(), &checked)) {
                if (checked) {
                    self->gameSettingsLedger_.set_desired(i, spec.defaultValue);
                } else {
                    self->gameSettingsLedger_.clear_desired(i);
                }
                self->gameSettingsDirty_.store(true, std::memory_order_release);
            }
            ImGui::SameLine();
            float value = enabled ? std::get<float>(*desired) : std::get<float>(spec.defaultValue);
            ImGui::SetNextItemWidth(120.0F);
            if (ImGui::InputFloat("##val", &value, 0.0F, 0.0F, "%.2f")) {
                const auto lo = std::get<float>(spec.minValue);
                const auto hi = std::get<float>(spec.maxValue);
                value = value < lo ? lo : (value > hi ? hi : value);
                self->gameSettingsLedger_.set_desired(i, value);
                self->gameSettingsDirty_.store(true, std::memory_order_release);
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", spec.description.data());
        }
    }
    if (headerOpen) {
        ImGui::TreePop();
    }

    if (ImGui::Button("全部恢复默认")) {
        self->gameSettingsLedger_.clear_all_desired();
        self->gameSettingsDirty_.store(true, std::memory_order_release);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("仅本进程有效；切图/卸载自动恢复原值。");
}
