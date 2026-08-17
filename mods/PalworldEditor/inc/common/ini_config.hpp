/**
 * @file ini_config.hpp
 * @brief ini `Key=Value` 配置的公共纯值解析原语。
 * @details 由远程终端、标记点传送等配置结构共用；只处理字符串，文件 IO 由调用方负责。
 */
#pragma once

#include <charconv>
#include <optional>
#include <string_view>
#include <system_error>

namespace pal_game {

/** @brief 键位必须是合法 VK 码（1–255）。 */
[[nodiscard]] inline auto valid_hotkey_vk(const int vk) noexcept -> bool {
    return vk >= 1 && vk <= 255;
}

/** @brief 解析布尔值；非 true/false 回退 fallback。 */
[[nodiscard]] inline auto parse_ini_bool(const std::string_view value, const bool fallback) noexcept
    -> bool {
    if (value == "true") {
        return true;
    }
    if (value == "false") {
        return false;
    }
    return fallback;
}

/** @brief 解析整数；失败返回空。 */
[[nodiscard]] inline auto parse_ini_int(const std::string_view value) -> std::optional<int> {
    int result{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size()) {
        return std::nullopt;
    }
    return result;
}

/** @brief 解析浮点；失败返回空。 */
[[nodiscard]] inline auto parse_ini_float(const std::string_view value) -> std::optional<float> {
    double result{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size()) {
        return std::nullopt;
    }
    return static_cast<float>(result);
}

}  // namespace pal_game
