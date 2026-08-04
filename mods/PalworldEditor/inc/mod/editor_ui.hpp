/**
 * @file editor_ui.hpp
 * @brief PalworldEditor 界面的共用样式与渲染辅助（纯 ImGui，不依赖 mod 状态）。
 */
#pragma once

namespace editor_ui {
/** @brief 应用深色现代主题到当前进程的 ImGui 全局样式（幂等，每帧调用）。 */
auto apply_editor_style() -> void;

/** @brief 渲染一个小标题 + 全宽分隔线，用作功能块边界。 */
auto section_header(const char* title) -> void;

/**
 * @brief RAII：在其作用域内把按钮临时切换为强调蓝（主操作）。
 * @details 构造时 PushStyleColor 三次，析构时 PopStyleColor(3)。默认按钮色保持中性灰。
 */
class scoped_accent_button {
public:
    scoped_accent_button();
    ~scoped_accent_button();
    scoped_accent_button(const scoped_accent_button&) = delete;
    auto operator=(const scoped_accent_button&) -> scoped_accent_button& = delete;
};
}  // namespace editor_ui
