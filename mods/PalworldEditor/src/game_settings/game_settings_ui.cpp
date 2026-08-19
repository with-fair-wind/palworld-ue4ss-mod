/**
 * @file game_settings_ui.cpp
 * @brief 游戏参数覆盖的 ImGui 列表渲染。
 */
#include <cstddef>
#include <variant>

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

    // 目录按分类排序：每个分类只创建一个 TreeNode，折叠时整组跳过。
    for (std::size_t i{}; i < game_settings::kOverrideCount;) {
        const auto category = game_settings::kOverrideCatalog[i].category;
        auto groupEnd = i;
        while (groupEnd < game_settings::kOverrideCount &&
               game_settings::kOverrideCatalog[groupEnd].category == category) {
            ++groupEnd;
        }

        if (ImGui::TreeNode(game_settings::category_name(category).data())) {
            for (; i < groupEnd; ++i) {
                const auto& spec = game_settings::kOverrideCatalog[i];
                // PushID 让每行的 "##val" 输入框 ID 唯一，避免 ImGui ID 冲突。
                ImGui::PushID(static_cast<int>(i));

                const auto desired = self->gameSettingsLedger_.desired(i);
                bool enabled = desired.has_value();
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
                    float value =
                        enabled ? std::get<float>(*desired) : std::get<float>(spec.defaultValue);
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
                ImGui::PopID();
            }
            ImGui::TreePop();
        } else {
            i = groupEnd;
        }
    }

    if (ImGui::Button("全部恢复默认")) {
        self->gameSettingsLedger_.clear_all_desired();
        self->gameSettingsDirty_.store(true, std::memory_order_release);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("仅本进程有效；切图/卸载自动恢复原值。");
}
