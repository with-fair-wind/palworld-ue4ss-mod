/**
 * @file current_base_resolution.hpp
 * @brief 定义当前据点反射路由及其纯值接受规则。
 */
#pragma once

#include <optional>
#include <string_view>

#include <base_resource_sharing/resource_pool.hpp>

namespace base_resource_sharing {
/**
 * @brief 为 UE4SS 字符类型提供唯一的当前据点反射名称来源。
 * @tparam Character UE4SS 反射 API 使用的字符类型。
 */
template <typename Character>
struct CurrentBaseReflectionNames;

template <>
struct CurrentBaseReflectionNames<char> {
    static constexpr std::string_view controllerPawnFunction{"K2_GetPawn"};
    static constexpr std::string_view insideComponentProperty{"InsideBaseCampCheckComponent"};
    static constexpr std::string_view insideBaseModelFunction{"GetInsideBaseCampModel"};
    static constexpr std::string_view baseIdProperty{"BaseCampId"};
};

template <>
struct CurrentBaseReflectionNames<wchar_t> {
    static constexpr std::wstring_view controllerPawnFunction{L"K2_GetPawn"};
    static constexpr std::wstring_view insideComponentProperty{L"InsideBaseCampCheckComponent"};
    static constexpr std::wstring_view insideBaseModelFunction{L"GetInsideBaseCampModel"};
    static constexpr std::wstring_view baseIdProperty{L"BaseCampId"};
};

/** @brief 当前据点解析不得以空间距离猜测据点。 */
inline constexpr bool kAllowsNearestBaseFallback{false};

/**
 * @brief 只接受有效且存在同公会普通仓储模块的当前据点。
 */
[[nodiscard]] constexpr auto accept_current_base(const GuidKey candidate,
                                                 const bool hasStorageModule) noexcept
    -> std::optional<GuidKey> {
    return candidate.valid() && hasStorageModule ? std::optional{candidate} : std::nullopt;
}
}  // namespace base_resource_sharing
