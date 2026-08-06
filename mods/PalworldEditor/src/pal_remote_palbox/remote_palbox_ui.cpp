/**
 * @file remote_palbox_ui.cpp
 * @brief 远程终端区的 ImGui 渲染：改键捕获、门控开关、测试按钮与状态行。
 * @details 只在 GUI 线程调用；所有修改经 RemotePalboxRuntime::set_config /
 *          request_open（内部加锁），不直接访问 Unreal 对象。
 */
#include <optional>
#include <string>

#include <imgui.h>
#include <mod/mod_core.hpp>
#include <pal_remote_palbox/remote_palbox_runtime.hpp>

namespace {

/** @brief 捕获模式是否激活；仅在 GUI 线程访问。 */
bool capturingHotkey = false;

/** @brief 把 ImGuiKey 映射为 Windows VK 码；无法映射返回 0。 */
auto imgui_key_to_vk(const ImGuiKey key) -> int {
    if (key >= ImGuiKey_A && key <= ImGuiKey_Z) {
        return static_cast<int>('A') + (static_cast<int>(key) - static_cast<int>(ImGuiKey_A));
    }
    if (key >= ImGuiKey_0 && key <= ImGuiKey_9) {
        return static_cast<int>('0') + (static_cast<int>(key) - static_cast<int>(ImGuiKey_0));
    }
    if (key >= ImGuiKey_F1 && key <= ImGuiKey_F24) {
        return static_cast<int>(VK_F1) + (static_cast<int>(key) - static_cast<int>(ImGuiKey_F1));
    }
    switch (key) {
        case ImGuiKey_Space:
            return VK_SPACE;
        case ImGuiKey_Enter:
            return VK_RETURN;
        case ImGuiKey_Tab:
            return VK_TAB;
        case ImGuiKey_Escape:
            return VK_ESCAPE;
        case ImGuiKey_Backspace:
            return VK_BACK;
        case ImGuiKey_Delete:
            return VK_DELETE;
        case ImGuiKey_Home:
            return VK_HOME;
        case ImGuiKey_End:
            return VK_END;
        case ImGuiKey_PageUp:
            return VK_PRIOR;
        case ImGuiKey_PageDown:
            return VK_NEXT;
        case ImGuiKey_LeftArrow:
            return VK_LEFT;
        case ImGuiKey_RightArrow:
            return VK_RIGHT;
        case ImGuiKey_UpArrow:
            return VK_UP;
        case ImGuiKey_DownArrow:
            return VK_DOWN;
        default:
            return 0;
    }
}

/** @brief VK 码的显示名（常见键；未知键显示 VK 数值）。 */
auto vk_display_name(const int vk) -> std::string {
    if (vk >= 'A' && vk <= 'Z') {
        return std::string(1, static_cast<char>(vk));
    }
    if (vk >= '0' && vk <= '9') {
        return std::string(1, static_cast<char>(vk));
    }
    if (vk >= VK_F1 && vk <= VK_F24) {
        return "F" + std::to_string(vk - VK_F1 + 1);
    }
    switch (vk) {
        case VK_SPACE:
            return "Space";
        case VK_RETURN:
            return "Enter";
        case VK_TAB:
            return "Tab";
        case VK_ESCAPE:
            return "Esc";
        case VK_BACK:
            return "Backspace";
        case VK_DELETE:
            return "Delete";
        case VK_LEFT:
            return "←";
        case VK_RIGHT:
            return "→";
        case VK_UP:
            return "↑";
        case VK_DOWN:
            return "↓";
        default:
            return "VK " + std::to_string(vk);
    }
}

/** @brief 渲染改键捕获按钮；捕获完成时写回配置并返回 true。 */
auto render_hotkey_capture(pal_remote_palbox::RemotePalboxRuntime& runtime,
                           const pal_remote_palbox::RemotePalboxConfig& config) -> bool {
    bool changed = false;
    if (!capturingHotkey) {
        if (ImGui::Button(("当前键位：" + vk_display_name(config.hotkeyVk)).c_str())) {
            capturingHotkey = true;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("（点击后按下新键）");
        return false;
    }

    ImGui::Text("正在等待按键…（Esc 取消）");
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        capturingHotkey = false;
        return false;
    }
    for (int key = static_cast<int>(ImGuiKey_NamedKey_BEGIN);
         key < static_cast<int>(ImGuiKey_NamedKey_END); ++key) {
        if (!ImGui::IsKeyPressed(static_cast<ImGuiKey>(key))) {
            continue;
        }
        const int vk = imgui_key_to_vk(static_cast<ImGuiKey>(key));
        if (vk == 0) {
            continue;
        }
        auto updated = config;
        updated.hotkeyVk = vk;
        runtime.set_config(updated);
        capturingHotkey = false;
        changed = true;
        break;
    }
    return changed;
}
}  // namespace

void PalworldEditorMod::render_remote_palbox(PalworldEditorMod* self) {
    auto& runtime = self->remotePalboxRuntime_;
    if (!ImGui::CollapsingHeader("远程终端")) {
        return;
    }

    const auto snapshot = runtime.snapshot();
    const bool hotkeyChanged = render_hotkey_capture(runtime, snapshot.config);
    if (hotkeyChanged) {
        return;  // 下一帧快照已更新
    }

    ImGui::Separator();
    bool dirty = false;
    auto config = snapshot.config;

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
