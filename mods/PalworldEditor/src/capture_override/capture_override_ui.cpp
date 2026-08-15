/**
 * @file capture_override_ui.cpp
 * @brief 捕获不可捕获帕鲁功能的 ImGui 两级开关渲染。
 * @details 只在 GUI 线程调用；切换开关只提交进程内原子请求，不直接访问 Unreal 对象。
 */
#include <imgui.h>
#include <mod/mod_core.hpp>

void PalworldEditorMod::render_capture_override(PalworldEditorMod* self) {
    ImGui::SeparatorText("捕获不可捕获的帕鲁");
    bool enabled = self->requestedCaptureEnabled_.load(std::memory_order_acquire);
    bool forceHundredPercent = self->requestedCaptureForcePercent_.load(std::memory_order_acquire);
    const auto phase = self->captureRuntimePhase_.load(std::memory_order_acquire);
    const auto safetyDisabled = phase == capture_override::CaptureRuntimePhase::safetyDisabled;

    ImGui::BeginDisabled(safetyDisabled);
    if (ImGui::Checkbox("解锁不可捕获帕鲁", &enabled)) {
        self->requestedCaptureEnabled_.store(enabled, std::memory_order_release);
        self->captureSettingDirty_.store(true, std::memory_order_release);
    }
    ImGui::BeginDisabled(!enabled);
    if (ImGui::Checkbox("强制 100% 成功率", &forceHundredPercent)) {
        self->requestedCaptureForcePercent_.store(forceHundredPercent, std::memory_order_release);
        self->captureSettingDirty_.store(true, std::memory_order_release);
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    if (phase == capture_override::CaptureRuntimePhase::hooksRegistered) {
        ImGui::TextColored(ImVec4(0.35F, 1.0F, 0.4F, 1.0F), "已启用：投球时实时清除捕获限制。");
    }
    if (safetyDisabled) {
        ImGui::TextColored(ImVec4(1.0F, 0.35F, 0.2F, 1.0F),
                           "Hook 签名、捕获字段或恢复事务失败；已在本世界安全停用。");
    }
    ImGui::TextDisabled("仅本次游戏进程有效；重新启动游戏后默认关闭。");
    ImGui::TextDisabled(
        "开启后投出的帕鲁球会临时清除目标的不可捕获/Boss 标志；关闭后新投出的球恢复原版判定。");
    ImGui::TextDisabled(
        "对人类 NPC（如商人）会临时翻转 IsPal 门控尽力解锁捕获；能否成功以游戏实际表现为准。");
}
