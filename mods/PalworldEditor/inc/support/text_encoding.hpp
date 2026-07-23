/**
 * @file text_encoding.hpp
 * @brief 提供 Windows UTF-16 与 UTF-8 之间的文本边界转换辅助函数。
 * @details 本文件封装 Windows API 的 UTF-16/UTF-8 转换边界，不涉及 Unreal 运行时或游戏对象。
 */
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <limits>
#include <string>
#include <string_view>

#include <Windows.h>

/**
 * @brief 定义 Windows 文本编码边界的轻量值转换工具。
 */
namespace text_encoding {
/**
 * @brief 将 UTF-16 宽字符串严格转换为 UTF-8。
 * @param[in] value 待转换的 UTF-16 文本视图；函数不保存该视图。
 * @return 转换后的 UTF-8 字符串。
 * @retval std::string{} 输入为空、长度超过 Win32 `int` 上限、包含非法 UTF-16，
 *         或 `WideCharToMultiByte` 未完整写入时返回空字符串。
 */
[[nodiscard]] inline auto to_utf8(const std::wstring_view value) -> std::string {
    if (value.empty()) {
        return {};
    }
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return {};
    }

    const auto sourceSize = static_cast<int>(value.size());
    const auto required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                              sourceSize, nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return {};
    }

    std::string result(static_cast<std::size_t>(required), '\0');
    const auto written = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                             sourceSize, result.data(), required, nullptr, nullptr);
    if (written != required) {
        return {};
    }
    return result;
}

/**
 * @brief 将 ASCII Raw ID 的每个字节扩展为宽字符。
 * @param[in] value 待扩展的 ASCII 标识符视图；函数不保存该视图。
 * @return 由输入字节逐一扩展得到的宽字符串。
 * @details 本函数仅适用于 ASCII 标识符，不是通用的 UTF-8 转 UTF-16 接口。
 */
[[nodiscard]] inline auto widen_ascii(const std::string_view value) -> std::wstring {
    std::wstring result;
    result.reserve(value.size());
    for (const auto character : value) {
        result.push_back(static_cast<wchar_t>(static_cast<unsigned char>(character)));
    }
    return result;
}
}  // namespace text_encoding
