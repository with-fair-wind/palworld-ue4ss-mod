/**
 * @file function_hook_registry.cpp
 * @brief 实现 UFunction 原生/Blueprint 脚本 Hook 的公共登记与对称注销。
 */
#include <algorithm>
#include <exception>
#include <ranges>
#include <utility>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/Hooks.hpp>
#include <Unreal/UObject.hpp>
#include <common/function_hook_registry.hpp>

namespace pal_game {
using namespace RC;
using namespace RC::Unreal;

namespace {
auto log_hook_error(const TCHAR* message) noexcept -> void {
    try {
        Output::send<LogLevel::Warning>(message);
    } catch (...) {
        // 卸载期间日志设备可能先于 Mod 关闭；回调钝化不能因此失败。
        static_cast<void>(0);
    }
}

auto unregister_native_hook(UFunction* function, const CallbackId callbackId) noexcept -> void {
    if (function == nullptr || callbackId < 0) {
        return;
    }
    try {
        static_cast<void>(function->UnregisterHook(callbackId));
    } catch (...) {
        log_hook_error(STR("PalworldEditor: native UFunction hook unregistration threw.\n"));
    }
}
}  // namespace

FunctionHookRegistry::FunctionHookRegistry(const std::wstring_view callbackNamePrefix)
    : callbackNamePrefix_{callbackNamePrefix} {}

FunctionHookRegistry::~FunctionHookRegistry() {
    for (const auto& binding : bindings_) {
        if (binding.gate != nullptr) {
            binding.gate->active.store(false, std::memory_order_release);
        }
    }
    if (scriptDispatcherGate_ != nullptr) {
        scriptDispatcherGate_->active.store(false, std::memory_order_release);
    }
    if (!bindings_.empty() || scriptPreCallbackId_ != Hook::ERROR_ID ||
        scriptPostCallbackId_ != Hook::ERROR_ID) {
        log_hook_error(
            STR("PalworldEditor: FunctionHookRegistry destroyed before game-thread hook "
                "unregistration; callbacks were made inert.\n"));
    }
}

auto FunctionHookRegistry::register_hook(UFunction* function, Callback preCallback,
                                         Callback postCallback) -> bool {
    if (function == nullptr || (!preCallback && !postCallback)) {
        return false;
    }

    const auto functionPointer = function->GetFuncPtr();
    const auto backend = select_function_hook_backend(
        functionPointer != nullptr,
        functionPointer == UObject::ProcessInternalInternal.get_function_address(),
        function->HasAnyFunctionFlags(EFunctionFlags::FUNC_Native));

    if (backend == FunctionHookBackend::scriptFunction) {
        if (!ensure_script_dispatcher_registered()) {
            return false;
        }
        try {
            bindings_.push_back({.function = function,
                                 .backend = backend,
                                 .gate = nullptr,
                                 .preCallback = std::move(preCallback),
                                 .postCallback = std::move(postCallback)});
        } catch (...) {
            const bool hasScriptBinding = std::ranges::any_of(bindings_, [](const auto& binding) {
                return binding.backend == FunctionHookBackend::scriptFunction;
            });
            if (!hasScriptBinding) {
                unregister_script_dispatcher();
            }
            return false;
        }
        return true;
    }
    if (backend != FunctionHookBackend::nativeFunction) {
        return false;
    }

    CallbackId preId{-1};
    CallbackId postId{-1};
    std::shared_ptr<CallbackGate> gate;
    try {
        gate = std::make_shared<CallbackGate>();
    } catch (...) {
        return false;
    }
    try {
        if (preCallback) {
            preId =
                function->RegisterPreHook([gate, callback = preCallback](
                                              UnrealScriptFunctionCallableContext& context, void*) {
                    if (gate->active.load(std::memory_order_acquire)) {
                        invoke_safely(callback, context);
                    }
                });
        }
        if (postCallback) {
            postId = function->RegisterPostHook(
                [gate, callback = postCallback](UnrealScriptFunctionCallableContext& context,
                                                void*) {
                    if (gate->active.load(std::memory_order_acquire)) {
                        invoke_safely(callback, context);
                    }
                });
        }
    } catch (...) {
        gate->active.store(false, std::memory_order_release);
        unregister_native_hook(function, preId);
        unregister_native_hook(function, postId);
        return false;
    }

    const bool preReady = !preCallback || preId >= 0;
    const bool postReady = !postCallback || postId >= 0;
    if (!preReady || !postReady) {
        gate->active.store(false, std::memory_order_release);
        unregister_native_hook(function, preId);
        unregister_native_hook(function, postId);
        return false;
    }

    try {
        bindings_.push_back({.function = function,
                             .backend = backend,
                             .gate = gate,
                             .preCallback = std::move(preCallback),
                             .postCallback = std::move(postCallback),
                             .preId = preId,
                             .postId = postId});
    } catch (...) {
        gate->active.store(false, std::memory_order_release);
        unregister_native_hook(function, preId);
        unregister_native_hook(function, postId);
        return false;
    }
    return true;
}

auto FunctionHookRegistry::unregister_all() noexcept -> void {
    for (const auto& binding : bindings_) {
        if (binding.gate != nullptr) {
            binding.gate->active.store(false, std::memory_order_release);
        }
    }
    if (scriptDispatcherGate_ != nullptr) {
        scriptDispatcherGate_->active.store(false, std::memory_order_release);
    }
    for (auto& binding : std::views::reverse(bindings_)) {
        if (binding.backend != FunctionHookBackend::nativeFunction || binding.function == nullptr) {
            continue;
        }
        unregister_native_hook(binding.function, binding.preId);
        unregister_native_hook(binding.function, binding.postId);
    }
    unregister_script_dispatcher();
    bindings_.clear();
    scriptDispatcherGate_.reset();
}

auto FunctionHookRegistry::empty() const noexcept -> bool {
    return bindings_.empty();
}

auto FunctionHookRegistry::ensure_script_dispatcher_registered() -> bool {
    if (scriptPreCallbackId_ != Hook::ERROR_ID && scriptPostCallbackId_ != Hook::ERROR_ID) {
        return true;
    }
    if (scriptDispatcherGate_ == nullptr ||
        !scriptDispatcherGate_->active.load(std::memory_order_acquire)) {
        try {
            scriptDispatcherGate_ = std::make_shared<CallbackGate>();
        } catch (...) {
            return false;
        }
    }

    try {
        const Hook::FCallbackOptions preOptions{
            .bOnce = false,
            .bReadonly = true,
            .OwnerModName = STR("PalworldEditor"),
            .HookName = callbackNamePrefix_ + STR("Pre"),
        };
        const auto gate = scriptDispatcherGate_;
        scriptPreCallbackId_ = Hook::RegisterProcessLocalScriptFunctionPreCallback(
            [this, gate](Hook::TCallbackIterationData<void>&, UObject* context, FFrame& stack,
                         void* result) {
                if (gate->active.load(std::memory_order_acquire)) {
                    dispatch_script(true, context, stack, result);
                }
            },
            preOptions);

        const Hook::FCallbackOptions postOptions{
            .bOnce = false,
            .bReadonly = true,
            .OwnerModName = STR("PalworldEditor"),
            .HookName = callbackNamePrefix_ + STR("Post"),
        };
        scriptPostCallbackId_ = Hook::RegisterProcessLocalScriptFunctionPostCallback(
            [this, gate](Hook::TCallbackIterationData<void>&, UObject* context, FFrame& stack,
                         void* result) {
                if (gate->active.load(std::memory_order_acquire)) {
                    dispatch_script(false, context, stack, result);
                }
            },
            postOptions);
    } catch (...) {
        unregister_script_dispatcher();
        return false;
    }

    if (scriptPreCallbackId_ != Hook::ERROR_ID && scriptPostCallbackId_ != Hook::ERROR_ID) {
        return true;
    }
    unregister_script_dispatcher();
    return false;
}

auto FunctionHookRegistry::unregister_script_dispatcher() noexcept -> void {
    if (scriptDispatcherGate_ != nullptr) {
        scriptDispatcherGate_->active.store(false, std::memory_order_release);
    }
    if (scriptPreCallbackId_ != Hook::ERROR_ID) {
        try {
            if (!Hook::UnregisterCallback(scriptPreCallbackId_)) {
                log_hook_error(
                    STR("PalworldEditor: failed to unregister script pre-hook callback\n"));
            }
        } catch (...) {
            log_hook_error(STR("PalworldEditor: script pre-hook unregistration threw.\n"));
        }
        scriptPreCallbackId_ = Hook::ERROR_ID;
    }
    if (scriptPostCallbackId_ != Hook::ERROR_ID) {
        try {
            if (!Hook::UnregisterCallback(scriptPostCallbackId_)) {
                log_hook_error(
                    STR("PalworldEditor: failed to unregister script post-hook callback\n"));
            }
        } catch (...) {
            log_hook_error(STR("PalworldEditor: script post-hook unregistration threw.\n"));
        }
        scriptPostCallbackId_ = Hook::ERROR_ID;
    }
}

auto FunctionHookRegistry::dispatch_script(const bool pre, UObject* context, FFrame& stack,
                                           void* result) -> void {
    auto* const function = stack.Node();
    for (const auto& binding : bindings_) {
        if (binding.backend != FunctionHookBackend::scriptFunction ||
            binding.function != function) {
            continue;
        }
        UnrealScriptFunctionCallableContext callableContext{context, stack, result};
        invoke_safely(pre ? binding.preCallback : binding.postCallback, callableContext);
    }
}

auto FunctionHookRegistry::invoke_safely(const Callback& callback,
                                         UnrealScriptFunctionCallableContext& context) -> void {
    if (!callback) {
        return;
    }
    try {
        callback(context, nullptr);
    } catch (const std::exception&) {
        log_hook_error(STR("PalworldEditor: UFunction hook callback rejected an exception\n"));
    } catch (...) {
        log_hook_error(
            STR("PalworldEditor: UFunction hook callback rejected an unknown exception\n"));
    }
}

}  // namespace pal_game
