/**
 * @file game_foreground.hpp
 * @brief 前台窗口归属判断（远程终端与标记传送共用）。
 * @details 两个模块的按键状态机都需要"游戏窗口在焦点上"判断；实现相同且契约稳定，
 *          提取为公共原语。只依赖 Win32 API。
 */
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace pal_game {

/** @brief 当前前台窗口是否属于本进程（游戏窗口在焦点上）。 */
[[nodiscard]] inline auto foreground_is_game() -> bool {
    auto* const foreground = GetForegroundWindow();
    if (foreground == nullptr) {
        return false;
    }
    DWORD pid{};
    GetWindowThreadProcessId(foreground, &pid);
    return pid == GetCurrentProcessId();
}

}  // namespace pal_game
