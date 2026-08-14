/**
 * @file function_hook_backend.hpp
 * @brief 提供 UFunction 原生/Blueprint 脚本 Hook 后端的纯值判定。
 */
#pragma once

#include <cstdint>

namespace pal_game {

/** @brief UFunction 可使用的回调后端。 */
enum class FunctionHookBackend : std::uint8_t {
    nativeFunction,
    scriptFunction,
    unsupported,
};

/**
 * @brief 根据运行时函数入口和标志选择 Hook 后端。
 * @param[in] hasFunctionPointer UFunction 是否具有底层调用入口。
 * @param[in] usesProcessInternal 底层入口是否为 Blueprint VM 的 ProcessInternal。
 * @param[in] nativeFlag UFunction 是否带 FUNC_Native。
 * @return 唯一安全后端；不一致组合返回 unsupported。
 */
[[nodiscard]] constexpr auto select_function_hook_backend(const bool hasFunctionPointer,
                                                          const bool usesProcessInternal,
                                                          const bool nativeFlag) noexcept
    -> FunctionHookBackend {
    if (!hasFunctionPointer) {
        return FunctionHookBackend::unsupported;
    }
    if (!usesProcessInternal && nativeFlag) {
        return FunctionHookBackend::nativeFunction;
    }
    if (usesProcessInternal && !nativeFlag) {
        return FunctionHookBackend::scriptFunction;
    }
    return FunctionHookBackend::unsupported;
}

}  // namespace pal_game
