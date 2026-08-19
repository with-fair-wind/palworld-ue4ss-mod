/**
 * @file hotkey_capture_ui.hpp
 * @brief "点击后按下新键"的改键捕获公共 ImGui 原语。
 * @details 由远程终端与标记点传送共用；GUI 线程专用，捕获结果为 Windows VK 码，
 *          由调用方写回各自配置。只依赖 ImGui 与 Win32 VK 常量，不接触 Unreal。
 */
#pragma once

#include <optional>
#include <string>

#include <imgui.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace hotkey_capture {

/** @brief 捕获状态；每个使用方各持一份（互不抢占对方的面板）。 */
struct State {
    bool capturing{false};
};

/** @brief 把 ImGuiKey 映射为 Windows VK 码；无法映射返回 0。 */
[[nodiscard]] inline auto imgui_key_to_vk(const ImGuiKey key) -> int {
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
[[nodiscard]] inline auto vk_display_name(const int vk) -> std::string {
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
        case VK_HOME:
            return "Home";
        case VK_END:
            return "End";
        case VK_PRIOR:
            return "PageUp";
        case VK_NEXT:
            return "PageDown";
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

/**
 * @brief 渲染改键捕获按钮。
 * @param[in,out] state 调用方持有的捕获状态。
 * @param[in] currentVk 当前键位 VK 码（用于按钮显示）。
 * @return 捕获到合法新键时返回其 VK 码（调用方写回配置）；未变化返回空。
 * @note Esc 取消捕获；仅接受可映射的命名键。
 */
inline auto render(State& state, const int currentVk) -> std::optional<int> {
    if (!state.capturing) {
        if (ImGui::Button(("当前键位：" + vk_display_name(currentVk)).c_str())) {
            state.capturing = true;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("（点击后按下新键）");
        return std::nullopt;
    }

    ImGui::Text("正在等待按键…（Esc 取消）");
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        state.capturing = false;
        return std::nullopt;
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
        state.capturing = false;
        return vk;
    }
    return std::nullopt;
}

}  // namespace hotkey_capture
