#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <limits>
#include <string>
#include <string_view>

namespace text_encoding
{
[[nodiscard]] inline auto to_utf8(const std::wstring_view value) -> std::string
{
    if (value.empty())
    {
        return {};
    }
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    {
        return {};
    }

    const auto sourceSize = static_cast<int>(value.size());
    const auto required =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), sourceSize, nullptr, 0, nullptr, nullptr);
    if (required <= 0)
    {
        return {};
    }

    std::string result(static_cast<std::size_t>(required), '\0');
    const auto written = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), sourceSize, result.data(), required, nullptr, nullptr);
    if (written != required)
    {
        return {};
    }
    return result;
}

[[nodiscard]] inline auto widen_ascii(const std::string_view value) -> std::wstring
{
    std::wstring result;
    result.reserve(value.size());
    for (const auto character : value)
    {
        result.push_back(static_cast<wchar_t>(static_cast<unsigned char>(character)));
    }
    return result;
}
}
