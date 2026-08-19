/**
 * @file waypoint_teleport_ui.cpp
 * @brief 传送至地图标记点的 ImGui 配置与状态渲染。
 * @details 只在 GUI 线程调用；修改配置经 set_config 线程安全提交并写回 ini，
 *          不直接访问 Unreal 对象。
 */
#include <common/hotkey_capture_ui.hpp>
#include <imgui.h>
#include <mod/mod_core.hpp>

void PalworldEditorMod::render_waypoint_teleport(PalworldEditorMod* self) {
    ImGui::SeparatorText("标记点传送");
    if (!ImGui::CollapsingHeader("传送设置", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }
    const auto snapshot = self->waypointTeleportRuntime_.snapshot();

    static hotkey_capture::State captureState;
    if (const auto newVk = hotkey_capture::render(captureState, snapshot.config.hotkeyVk)) {
        auto updated = snapshot.config;
        updated.hotkeyVk = *newVk;
        self->waypointTeleportRuntime_.set_config(updated);
        return;  // 下一帧快照已更新
    }
    auto config = snapshot.config;
    if (ImGui::Checkbox("骑乘时禁用", &config.disableWhileMounted)) {
        self->waypointTeleportRuntime_.set_config(config);
    }
    if (ImGui::Checkbox("地牢内禁用", &config.disableInDungeon)) {
        self->waypointTeleportRuntime_.set_config(config);
    }
    if (ImGui::Checkbox("战斗中禁用", &config.disableDuringCombat)) {
        self->waypointTeleportRuntime_.set_config(config);
    }
    float arrivalOffset = config.arrivalHeightOffset;
    if (ImGui::InputFloat("到达高度偏移(cm)", &arrivalOffset)) {
        config.arrivalHeightOffset = waypoint_teleport::valid_arrival_offset(arrivalOffset)
                                         ? arrivalOffset
                                         : config.arrivalHeightOffset;
        self->waypointTeleportRuntime_.set_config(config);
    }

    ImGui::BeginDisabled(snapshot.domainDisabled);
    if (ImGui::Button("立即传送到最近标记")) {
        self->waypointTeleportRuntime_.request_teleport();
    }
    ImGui::EndDisabled();

    if (!snapshot.lastMessage.empty()) {
        ImGui::TextWrapped("%s", snapshot.lastMessage.c_str());
    }
    if (snapshot.domainDisabled) {
        ImGui::TextColored(ImVec4(1.0F, 0.35F, 0.2F, 1.0F),
                           "标记/传送结构不兼容；本世界已停用标记传送。");
    }
    ImGui::TextDisabled(
        "默认 F7；点击上方按钮后按下新键即可改键；设置保存在 waypoint_teleport.ini。");
    ImGui::TextDisabled(
        "传送到距离最近的自定义地图标记；传送不会删除或修改标记，需连续传送时请在地图中手动删除。");
    ImGui::TextDisabled("成功 %llu 次；失败/拦截 %llu 次。",
                        static_cast<unsigned long long>(snapshot.teleportCount),
                        static_cast<unsigned long long>(snapshot.failCount));
}
