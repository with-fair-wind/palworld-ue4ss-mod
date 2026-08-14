/**
 * @file function_hook_registry.hpp
 * @brief 声明同时支持原生 UFunction 与 Blueprint 脚本函数的可逆 Hook 登记器。
 */
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <common/function_hook_backend.hpp>

namespace RC::Unreal {
class UFunction;
}

namespace pal_game {

/**
 * @brief 管理一组 UFunction pre/post Hook，并统一处理 Blueprint 脚本分发。
 * @details 登记器只保存 Hook 生命周期所需的 UFunction 非拥有句柄；调用者必须在 LoadMap
 *          前或所属模块卸载前调用 unregister_all()。单次注册失败会立即回滚本次登记。
 */
class FunctionHookRegistry final {
public:
    using Callback = RC::Unreal::UnrealScriptFunctionCallable;

    /** @param[in] callbackNamePrefix 全局脚本回调的诊断名称前缀。 */
    explicit FunctionHookRegistry(std::wstring_view callbackNamePrefix);
    ~FunctionHookRegistry();
    FunctionHookRegistry(const FunctionHookRegistry&) = delete;
    auto operator=(const FunctionHookRegistry&) -> FunctionHookRegistry& = delete;
    FunctionHookRegistry(FunctionHookRegistry&&) = delete;
    auto operator=(FunctionHookRegistry&&) -> FunctionHookRegistry& = delete;

    /**
     * @brief 为一个已完成签名校验的 UFunction 登记回调。
     * @param[in] function 运行时 UFunction 非拥有句柄。
     * @param[in] preCallback 可为空的 pre-hook。
     * @param[in] postCallback 可为空的 post-hook。
     * @retval true 已完整登记。
     * @retval false 后端不受支持或登记失败；不会遗留本次部分登记。
     */
    [[nodiscard]] auto register_hook(RC::Unreal::UFunction* function, Callback preCallback,
                                     Callback postCallback) -> bool;

    /** @brief 逆序注销全部原生 Hook 与全局脚本分发回调。 */
    auto unregister_all() -> void;

    /** @retval true 当前没有已登记 Hook。 */
    [[nodiscard]] auto empty() const noexcept -> bool;

private:
    struct Binding {
        RC::Unreal::UFunction* function{};
        FunctionHookBackend backend{FunctionHookBackend::unsupported};
        Callback preCallback;
        Callback postCallback;
        RC::Unreal::CallbackId preId{-1};
        RC::Unreal::CallbackId postId{-1};
    };

    [[nodiscard]] auto ensure_script_dispatcher_registered() -> bool;
    auto unregister_script_dispatcher() -> void;
    auto dispatch_script(bool pre, RC::Unreal::UObject* context, RC::Unreal::FFrame& stack,
                         void* result) -> void;
    static auto invoke_safely(const Callback& callback,
                              RC::Unreal::UnrealScriptFunctionCallableContext& context) -> void;

    std::wstring callbackNamePrefix_;
    std::vector<Binding> bindings_;
    RC::Unreal::Hook::GlobalCallbackId scriptPreCallbackId_{RC::Unreal::Hook::ERROR_ID};
    RC::Unreal::Hook::GlobalCallbackId scriptPostCallbackId_{RC::Unreal::Hook::ERROR_ID};
};

}  // namespace pal_game
