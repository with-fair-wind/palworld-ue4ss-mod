/**
 * @file ini_config.hpp
 * @brief ini `Key=Value` 配置的公共纯值解析原语。
 * @details 由远程终端、标记点传送等配置结构共用；只处理字符串，文件 IO 由调用方负责。
 */
#pragma once

#include <algorithm>
#include <charconv>
#include <optional>
#include <string_view>
#include <system_error>

namespace pal_game {

/** @brief 去除键值首尾的空白字符（含 CRLF，兼容 Windows 编辑器行尾）。 */
[[nodiscard]] inline auto trim_ini_value(const std::string_view value) noexcept
    -> std::string_view {
    const auto is_space = [](const char c) noexcept {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    const auto begin = std::find_if_not(value.begin(), value.end(), is_space);
    const auto end = std::find_if_not(value.rbegin(), value.rend(), is_space).base();
    return begin < end ? std::string_view{begin, end} : std::string_view{};
}

/** @brief 键位必须是合法 VK 码（1–255）。 */
[[nodiscard]] inline auto valid_hotkey_vk(const int vk) noexcept -> bool {
    return vk >= 1 && vk <= 255;
}

/** @brief 解析布尔值；非 true/false（含带空白/CRLF 的写法）回退 fallback。 */
[[nodiscard]] inline auto parse_ini_bool(const std::string_view value, const bool fallback) noexcept
    -> bool {
    const auto trimmed = trim_ini_value(value);
    if (trimmed == "true") {
        return true;
    }
    if (trimmed == "false") {
        return false;
    }
    return fallback;
}

/** @brief 解析整数；失败（含空白/空串/CRLF）返回空。
 *  @note 空输入须先短路：默认构造的 string_view 的 data() 为空指针，
 *        继续做指针算术属于未定义行为。 */
[[nodiscard]] inline auto parse_ini_int(const std::string_view value) -> std::optional<int> {
    const auto trimmed = trim_ini_value(value);
    if (trimmed.empty()) {
        return std::nullopt;
    }
    int result{};
    const auto [end, error] =
        std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), result);
    if (error != std::errc{} || end != trimmed.data() + trimmed.size()) {
        return std::nullopt;
    }
    return result;
}

/** @brief 解析浮点；失败（含空白/空串/CRLF）返回空。
 *  @note 空输入短路理由同 parse_ini_int。 */
[[nodiscard]] inline auto parse_ini_float(const std::string_view value) -> std::optional<float> {
    const auto trimmed = trim_ini_value(value);
    if (trimmed.empty()) {
        return std::nullopt;
    }
    double result{};
    const auto [end, error] =
        std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), result);
    if (error != std::errc{} || end != trimmed.data() + trimmed.size()) {
        return std::nullopt;
    }
    return static_cast<float>(result);
}

}  // namespace pal_game
