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

auto unregister_native_hook(UFunction* function, CallbackId& callbackId) noexcept -> bool {
    if (function == nullptr || callbackId < 0) {
        return true;  // 无可注销项视为成功。
    }
    try {
        if (!function->UnregisterHook(callbackId)) {
            return false;
        }
    } catch (...) {
        log_hook_error(STR("PalworldEditor: native UFunction hook unregistration threw.\n"));
        return false;
    }
    callbackId = Hook::ERROR_ID;  // 仅在确认注销成功后丢弃 id，失败保留供重试。
    return true;
}

/** @brief 登记失败时回滚刚注册的原生 Hook；回滚不彻底只记日志（绑定尚未建立）。 */
auto rollback_native_hooks(UFunction* function, CallbackId& preId, CallbackId& postId) noexcept
    -> void {
    const bool preOk = unregister_native_hook(function, preId);
    const bool postOk = unregister_native_hook(function, postId);
    if (!preOk || !postOk) {
        log_hook_error(
            STR("PalworldEditor: rollback after failed registration left a native hook "
                "behind (gate deactivated)\n"));
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
        rollback_native_hooks(function, preId, postId);
        return false;
    }

    const bool preReady = !preCallback || preId >= 0;
    const bool postReady = !postCallback || postId >= 0;
    if (!preReady || !postReady) {
        gate->active.store(false, std::memory_order_release);
        rollback_native_hooks(function, preId, postId);
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
        rollback_native_hooks(function, preId, postId);
        return false;
    }
    return true;
}

auto FunctionHookRegistry::unregister_all() noexcept -> std::size_t {
    // 钝化优先：无论注销成败，所有回调门先关闭，残留注册只会空转闭包的 gate 检查。
    for (const auto& binding : bindings_) {
        if (binding.gate != nullptr) {
            binding.gate->active.store(false, std::memory_order_release);
        }
    }
    if (scriptDispatcherGate_ != nullptr) {
        scriptDispatcherGate_->active.store(false, std::memory_order_release);
    }
    const bool dispatcherWasRegistered = scriptPreCallbackId_ != Hook::ERROR_ID ||
                                         scriptPostCallbackId_ != Hook::ERROR_ID ||
                                         scriptDispatcherGate_ != nullptr;
    const bool scriptOk = unregister_script_dispatcher();
    // 逆序尝试注销原生 Hook；部分注销的绑定只保留仍注册的 id，完整失败保留整条绑定。
    for (std::size_t index = bindings_.size(); index > 0; --index) {
        auto& binding = bindings_[index - 1];
        if (binding.backend != FunctionHookBackend::nativeFunction || binding.function == nullptr) {
            continue;  // 脚本后端绑定随全局分发器的注销失效。
        }
        static_cast<void>(unregister_native_hook(binding.function, binding.preId));
        static_cast<void>(unregister_native_hook(binding.function, binding.postId));
    }
    // 压缩掉已完整注销的绑定；失败项留在登记器中供下次 unregister_all 重试。
    std::size_t writeIndex{0};
    for (std::size_t readIndex = 0; readIndex < bindings_.size(); ++readIndex) {
        auto& binding = bindings_[readIndex];
        const bool removable =
            binding.backend != FunctionHookBackend::nativeFunction
                ? scriptOk
                : binding.function == nullptr || (binding.preId < 0 && binding.postId < 0);
        if (removable) {
            continue;
        }
        if (writeIndex != readIndex) {
            bindings_[writeIndex] = std::move(binding);
        }
        ++writeIndex;
    }
    bindings_.resize(writeIndex);
    return bindings_.size() + ((dispatcherWasRegistered && !scriptOk) ? 1 : 0);
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

auto FunctionHookRegistry::unregister_script_dispatcher() noexcept -> bool {
    if (scriptDispatcherGate_ != nullptr) {
        scriptDispatcherGate_->active.store(false, std::memory_order_release);
    }
    bool allOk{true};
    if (scriptPreCallbackId_ != Hook::ERROR_ID) {
        bool ok{false};
        try {
            ok = Hook::UnregisterCallback(scriptPreCallbackId_);
            if (!ok) {
                log_hook_error(
                    STR("PalworldEditor: failed to unregister script pre-hook callback\n"));
            }
        } catch (...) {
            log_hook_error(STR("PalworldEditor: script pre-hook unregistration threw.\n"));
        }
        if (ok) {
            scriptPreCallbackId_ = Hook::ERROR_ID;  // 确认成功才丢弃 id，失败保留供重试。
        }
        allOk = allOk && ok;
    }
    if (scriptPostCallbackId_ != Hook::ERROR_ID) {
        bool ok{false};
        try {
            ok = Hook::UnregisterCallback(scriptPostCallbackId_);
            if (!ok) {
                log_hook_error(
                    STR("PalworldEditor: failed to unregister script post-hook callback\n"));
            }
        } catch (...) {
            log_hook_error(STR("PalworldEditor: script post-hook unregistration threw.\n"));
        }
        if (ok) {
            scriptPostCallbackId_ = Hook::ERROR_ID;
        }
        allOk = allOk && ok;
    }
    if (allOk) {
        scriptDispatcherGate_.reset();  // 失败时保留已钝化的门，重试路径复用。
    }
    return allOk;
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
