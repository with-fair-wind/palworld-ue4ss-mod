/**
 * @file revive_timer_ui.cpp
 * @brief 终端复活计时移除开关的 ImGui 渲染实现。
 * @details 只在 GUI 线程调用；切换开关只提交进程内原子请求，不直接访问 Unreal 对象。
 */
#include <mutex>
#include <string>

#include <imgui.h>
#include <mod/mod_core.hpp>
#include <revive_timer/revive_timer_gateway.hpp>

void PalworldEditorMod::render_revive_timer(PalworldEditorMod* self) {
    ImGui::SeparatorText("终端复活计时");
    bool enabled = self->requestedReviveTimerRemove_.load(std::memory_order_acquire);
    const auto phase = self->reviveTimerPhase_.load(std::memory_order_acquire);
    const auto safetyDisabled = phase == revive_timer::ReviveTimerRuntimePhase::safetyDisabled ||
                                self->reviveTimerLedger_.safety_disabled();
    ImGui::BeginDisabled(safetyDisabled);
    if (ImGui::Checkbox("移除帕鲁终端复活等待", &enabled)) {
        self->requestedReviveTimerRemove_.store(enabled, std::memory_order_release);
        self->reviveTimerSettingDirty_.store(true, std::memory_order_release);
    }
    ImGui::EndDisabled();
    if (phase == revive_timer::ReviveTimerRuntimePhase::waitingForRetry &&
        ImGui::Button("重新检测游戏设置")) {
        self->reviveTimerRetryRequested_.store(true, std::memory_order_release);
    }

    std::string runtimeStatus;
    {
        const std::lock_guard lock(self->reviveTimerStatusMutex_);
        runtimeStatus = self->reviveTimerStatus_;
    }
    if (!runtimeStatus.empty()) {
        ImGui::TextWrapped("%s", runtimeStatus.c_str());
    }
    if (phase == revive_timer::ReviveTimerRuntimePhase::waitingForRetry) {
        ImGui::TextColored(ImVec4(1.0F, 0.75F, 0.2F, 1.0F),
                           "世界或游戏设置尚未就绪；进入存档后请点击“重新检测游戏设置”。");
    }
    if (safetyDisabled) {
        ImGui::TextColored(ImVec4(1.0F, 0.35F, 0.2F, 1.0F),
                           "字段布局、写入验证或恢复失败；已安全停用，切换开关不会绕过。");
    }
    ImGui::TextDisabled("仅本次游戏进程有效；关闭开关、切图与卸载时恢复原值。");
}
