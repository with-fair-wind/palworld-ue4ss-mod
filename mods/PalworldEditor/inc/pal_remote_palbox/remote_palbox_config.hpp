/**
 * @file remote_palbox_config.hpp
 * @brief 远程终端 ini 配置的纯值解析与序列化。
 * @details 只处理字符串；文件 IO 由运行时层负责。
 */
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include <common/ini_config.hpp>
namespace pal_remote_palbox {

/** @brief 远程终端运行时配置；与 remote_palbox.ini 的键一一对应。 */
struct RemotePalboxConfig {
    int hotkeyVk{74};                 /**< 打开快捷键 VK 码；默认 J（0x4A=74）。 */
    bool disableWhileMounted{true};   /**< 骑乘时禁用。 */
    bool disableInDungeon{true};      /**< 地牢内禁用。 */
    bool onlyInsideBaseCircle{false}; /**< 仅基地圈内可用。 */
    bool disableDuringCombat{false};  /**< 战斗中禁用。 */
};

inline constexpr RemotePalboxConfig kDefaultRemotePalboxConfig{};

/**
 * @brief 按行解析 `Key=Value` 配置。
 * @details 未知键忽略；`HotkeyVk` 非 1–255 整数回退 74；其余键非法值回退默认。
 */
[[nodiscard]] inline auto parse_remote_palbox_config(const std::string_view content)
    -> RemotePalboxConfig {
    RemotePalboxConfig config = kDefaultRemotePalboxConfig;
    std::size_t pos{};
    while (pos < content.size()) {
        const auto eol = content.find('\n', pos);
        const auto line =
            content.substr(pos, eol == std::string_view::npos ? content.size() - pos : eol - pos);
        pos = eol == std::string_view::npos ? content.size() : eol + 1;
        const auto eq = line.find('=');
        if (eq == std::string_view::npos || eq == 0) {
            continue;
        }
        const auto key = line.substr(0, eq);
        const auto value = line.substr(eq + 1);
        if (key == "HotkeyVk") {
            const auto parsed = pal_game::parse_ini_int(value);
            if (parsed.has_value() && pal_game::valid_hotkey_vk(*parsed)) {
                config.hotkeyVk = *parsed;
            }
        } else if (key == "DisableWhileMounted") {
            config.disableWhileMounted =
                pal_game::parse_ini_bool(value, config.disableWhileMounted);
        } else if (key == "DisableInDungeon") {
            config.disableInDungeon = pal_game::parse_ini_bool(value, config.disableInDungeon);
        } else if (key == "OnlyInsideBaseCircle") {
            config.onlyInsideBaseCircle =
                pal_game::parse_ini_bool(value, config.onlyInsideBaseCircle);
        } else if (key == "DisableDuringCombat") {
            config.disableDuringCombat =
                pal_game::parse_ini_bool(value, config.disableDuringCombat);
        }
        // 未知键忽略。
    }
    return config;
}

/** @brief 固定键序序列化，与 parse 互逆。 */
[[nodiscard]] inline auto serialize_remote_palbox_config(const RemotePalboxConfig& config)
    -> std::string {
    return "HotkeyVk=" + std::to_string(config.hotkeyVk) +
           "\n"
           "DisableWhileMounted=" +
           (config.disableWhileMounted ? "true" : "false") +
           "\n"
           "DisableInDungeon=" +
           (config.disableInDungeon ? "true" : "false") +
           "\n"
           "OnlyInsideBaseCircle=" +
           (config.onlyInsideBaseCircle ? "true" : "false") +
           "\n"
           "DisableDuringCombat=" +
           (config.disableDuringCombat ? "true" : "false") + "\n";
}
}  // namespace pal_remote_palbox
