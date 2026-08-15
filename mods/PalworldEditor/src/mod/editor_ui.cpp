/**
 * @file editor_ui.cpp
 * @brief 实现 PalworldEditor 的深色现代主题、分组辅助与主窗口 Tab 编排。
 */
#include <imgui.h>
#include <mod/editor_ui.hpp>
#include <mod/mod_core.hpp>

namespace {
/** @brief 把 0xRRGGBB 转成 alpha=1 的 ImVec4。 */
ImVec4 rgb(const unsigned hex) {
    return ImVec4{static_cast<float>((hex >> 16) & 0xFF) / 255.0F,
                  static_cast<float>((hex >> 8) & 0xFF) / 255.0F,
                  static_cast<float>(hex & 0xFF) / 255.0F, 1.0F};
}
}  // namespace

namespace editor_ui {
auto apply_editor_style() -> void {
    ImGuiStyle& style = ImGui::GetStyle();
    ImGui::StyleColorsDark(&style);
    ImVec4* const c = style.Colors;
    c[ImGuiCol_WindowBg] = rgb(0x1f1f1f);
    c[ImGuiCol_ChildBg] = rgb(0x1f1f1f);
    c[ImGuiCol_PopupBg] = rgb(0x1f1f1f);
    c[ImGuiCol_Border] = rgb(0x3a3a3a);
    c[ImGuiCol_Separator] = rgb(0x3a3a3a);
    c[ImGuiCol_FrameBg] = rgb(0x2d2d2d);
    c[ImGuiCol_FrameBgHovered] = rgb(0x383838);
    c[ImGuiCol_FrameBgActive] = rgb(0x3a3a3a);
    // 默认按钮中性灰；主操作用 scoped_accent_button 局部套强调蓝。
    c[ImGuiCol_Button] = rgb(0x3a3a3a);
    c[ImGuiCol_ButtonHovered] = rgb(0x484848);
    c[ImGuiCol_ButtonActive] = rgb(0x2d2d2d);
    c[ImGuiCol_Header] = rgb(0x3a7afe);
    c[ImGuiCol_HeaderHovered] = rgb(0x33415c);
    c[ImGuiCol_HeaderActive] = rgb(0x33415c);
    c[ImGuiCol_Tab] = rgb(0x2d2d2d);
    c[ImGuiCol_TabHovered] = rgb(0x383838);
    c[ImGuiCol_TabActive] = rgb(0x323232);
    c[ImGuiCol_TabUnfocused] = rgb(0x1f1f1f);
    c[ImGuiCol_TabUnfocusedActive] = rgb(0x2d2d2d);
    c[ImGuiCol_Text] = rgb(0xe0e0e0);
    c[ImGuiCol_TextDisabled] = rgb(0x888888);
    c[ImGuiCol_ScrollbarBg] = rgb(0x1f1f1f);
    c[ImGuiCol_ScrollbarGrab] = rgb(0x3a3a3a);
    c[ImGuiCol_ScrollbarGrabHovered] = rgb(0x4a4a4a);
    c[ImGuiCol_ScrollbarGrabActive] = rgb(0x4a4a4a);
    style.WindowRounding = 8.0F;
    style.ChildRounding = 6.0F;
    style.PopupRounding = 6.0F;
    style.FrameRounding = 6.0F;
    style.GrabRounding = 6.0F;
    style.TabRounding = 6.0F;
    style.ScrollbarRounding = 8.0F;
    style.WindowPadding = ImVec2(12.0F, 12.0F);
    style.FramePadding = ImVec2(8.0F, 4.0F);
    style.ItemSpacing = ImVec2(8.0F, 6.0F);
    style.ItemInnerSpacing = ImVec2(6.0F, 4.0F);
    style.IndentSpacing = 18.0F;
    style.ScrollbarSize = 13.0F;
    style.AntiAliasedLines = true;
}

auto section_header(const char* title) -> void {
    ImGui::TextUnformatted(title);
    ImGui::Separator();
}

scoped_accent_button::scoped_accent_button() {
    ImGui::PushStyleColor(ImGuiCol_Button, rgb(0x3a7afe));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, rgb(0x5a8fff));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, rgb(0x2e6ae0));
}

scoped_accent_button::~scoped_accent_button() {
    ImGui::PopStyleColor(3);
}
}  // namespace editor_ui

void PalworldEditorMod::render_main_window(PalworldEditorMod* self) {
    editor_ui::apply_editor_style();
    ImGui::SetNextWindowSize(ImVec2(580.0F, 640.0F), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("PalworldEditor v1.6.10", nullptr)) {
        ImGui::End();
        return;
    }
    const bool runtimeDisabled = self->runtimeSafetyDisabled_.load(std::memory_order_acquire);
    if (runtimeDisabled) {
        ImGui::TextColored(
            ImVec4(1.0F, 0.35F, 0.2F, 1.0F),
            "运行时因生命周期或异常错误已安全停用；请查看 UE4SS 日志并重新加载 Mod。");
    }
    ImGui::BeginDisabled(runtimeDisabled);
    if (ImGui::BeginTabBar("##editor_tabs", ImGuiTabBarFlags_None)) {
        if (ImGui::BeginTabItem("物品")) {
            render_stack_unlimited(self);
            render_give_items(self);
            render_item_browser(self);
            render_inventory(self);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("帕鲁")) {
            render_pal_editor(self);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("据点")) {
            render_base_resource_sharing(self);
            render_remote_palbox(self);
            render_grapple_no_cooldown(self);
            render_capture_override(self);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("诊断")) {
            render_diagnostics(self);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndDisabled();
    ImGui::End();
}

void PalworldEditorMod::render_diagnostics(PalworldEditorMod* self) {
    editor_ui::section_header("诊断");
    if (ImGui::Button("发现对象")) {
        self->want_discover_.store(true);
    }
}
