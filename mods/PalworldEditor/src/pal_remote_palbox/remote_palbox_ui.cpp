/**
 * @file remote_palbox_ui.cpp
 * @brief 远程终端区的 ImGui 渲染：改键捕获、门控开关、测试按钮与状态行。
 * @details 只在 GUI 线程调用；所有修改经 RemotePalboxRuntime::set_config /
 *          request_open（内部加锁），不直接访问 Unreal 对象。
 */
#include <string>

#include <common/hotkey_capture_ui.hpp>
#include <imgui.h>
#include <mod/mod_core.hpp>
#include <pal_remote_palbox/remote_palbox_runtime.hpp>

void PalworldEditorMod::render_remote_palbox(PalworldEditorMod* self) {
    auto& runtime = self->remotePalboxRuntime_;
    if (!ImGui::CollapsingHeader("远程终端")) {
        return;
    }

    const auto snapshot = runtime.snapshot();
    static hotkey_capture::State captureState;
    if (const auto newVk = hotkey_capture::render(captureState, snapshot.config.hotkeyVk)) {
        auto updated = snapshot.config;
        updated.hotkeyVk = *newVk;
        runtime.set_config(updated);
        return;  // 下一帧快照已更新
    }

    ImGui::Separator();
    bool dirty = false;
    auto config = snapshot.config;
    ImGui::PushID("remote_palbox_gates");

    if (ImGui::Checkbox("骑乘时禁用", &config.disableWhileMounted)) {
        dirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("地牢内禁用（原版地牢内打开存在崩溃风险）", &config.disableInDungeon)) {
        dirty = true;
    }
    if (ImGui::Checkbox("战斗中禁用", &config.disableDuringCombat)) {
        dirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("仅基地圈内可用", &config.onlyInsideBaseCircle)) {
        dirty = true;
    }
    ImGui::PopID();

    if (dirty) {
        runtime.set_config(config);
    }

    ImGui::Separator();
    if (ImGui::Button("立即打开终端（测试）")) {
        runtime.request_open();
    }
    ImGui::SameLine();
    ImGui::Text("成功 %llu / 失败 %llu", static_cast<unsigned long long>(snapshot.openCount),
                static_cast<unsigned long long>(snapshot.failCount));

    if (snapshot.domainDisabled) {
        ImGui::TextColored(ImVec4{1.0F, 0.4F, 0.4F, 1.0F}, "远程终端已停用（本世界）");
    }
    if (!snapshot.lastMessage.empty()) {
        ImGui::TextWrapped("%s", snapshot.lastMessage.c_str());
    }
}
