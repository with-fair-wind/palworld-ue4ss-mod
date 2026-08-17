/**
 * @file capture_override_ui.cpp
 * @brief 捕获覆盖功能的 ImGui 双开关渲染（解锁与强制相互独立）。
 * @details 只在 GUI 线程调用；切换开关只提交进程内原子请求，不直接访问 Unreal 对象。
 */
#include <imgui.h>
#include <mod/mod_core.hpp>

void PalworldEditorMod::render_capture_override(PalworldEditorMod* self) {
    ImGui::SeparatorText("捕获覆盖");
    bool unlock = self->requestedCaptureUnlock_.load(std::memory_order_acquire);
    bool forceHundredPercent = self->requestedCaptureForcePercent_.load(std::memory_order_acquire);
    const auto phase = self->captureRuntimePhase_.load(std::memory_order_acquire);
    const auto safetyDisabled = phase == capture_override::CaptureRuntimePhase::safetyDisabled;

    ImGui::BeginDisabled(safetyDisabled);
    if (ImGui::Checkbox("解锁不可捕获目标", &unlock)) {
        self->requestedCaptureUnlock_.store(unlock, std::memory_order_release);
        self->captureSettingDirty_.store(true, std::memory_order_release);
    }
    if (ImGui::Checkbox("强制 100% 成功率", &forceHundredPercent)) {
        self->requestedCaptureForcePercent_.store(forceHundredPercent, std::memory_order_release);
        self->captureSettingDirty_.store(true, std::memory_order_release);
    }
    ImGui::EndDisabled();

    if (phase == capture_override::CaptureRuntimePhase::hooksRegistered) {
        ImGui::TextColored(ImVec4(0.35F, 1.0F, 0.4F, 1.0F), "已启用：投球时实时覆盖捕获判定。");
    }
    if (safetyDisabled) {
        ImGui::TextColored(ImVec4(1.0F, 0.35F, 0.2F, 1.0F),
                           "Hook 签名、捕获字段或恢复事务失败；已在本世界安全停用。");
    }
    ImGui::TextDisabled("两个开关相互独立，仅本次游戏进程有效；重新启动游戏后默认关闭。");
    ImGui::TextDisabled(
        "解锁：临时清除目标的不可捕获/Boss 标志（含人类 NPC 的 IsPal 门控，尽力支持）。");
    ImGui::TextDisabled("强制：临时改写捕获成功率，普通帕鲁必捕；不解锁不可捕获目标。");
}
