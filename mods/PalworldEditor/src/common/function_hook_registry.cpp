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

FunctionHookRegistry::FunctionHookRegistry(const std::wstring_view callbackNamePrefix)
    : callbackNamePrefix_{callbackNamePrefix} {}

FunctionHookRegistry::~FunctionHookRegistry() {
    unregister_all();
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
    if (preCallback) {
        preId = function->RegisterPreHook(
            [callback = preCallback](UnrealScriptFunctionCallableContext& context, void*) {
                invoke_safely(callback, context);
            });
    }
    if (postCallback) {
        postId = function->RegisterPostHook(
            [callback = postCallback](UnrealScriptFunctionCallableContext& context, void*) {
                invoke_safely(callback, context);
            });
    }

    const bool preReady = !preCallback || preId >= 0;
    const bool postReady = !postCallback || postId >= 0;
    if (!preReady || !postReady) {
        if (preId >= 0) {
            static_cast<void>(function->UnregisterHook(preId));
        }
        if (postId >= 0) {
            static_cast<void>(function->UnregisterHook(postId));
        }
        return false;
    }

    try {
        bindings_.push_back({.function = function,
                             .backend = backend,
                             .preCallback = std::move(preCallback),
                             .postCallback = std::move(postCallback),
                             .preId = preId,
                             .postId = postId});
    } catch (...) {
        if (preId >= 0) {
            static_cast<void>(function->UnregisterHook(preId));
        }
        if (postId >= 0) {
            static_cast<void>(function->UnregisterHook(postId));
        }
        return false;
    }
    return true;
}

auto FunctionHookRegistry::unregister_all() -> void {
    for (auto& binding : std::views::reverse(bindings_)) {
        if (binding.backend != FunctionHookBackend::nativeFunction || binding.function == nullptr) {
            continue;
        }
        if (binding.preId >= 0) {
            static_cast<void>(binding.function->UnregisterHook(binding.preId));
        }
        if (binding.postId >= 0) {
            static_cast<void>(binding.function->UnregisterHook(binding.postId));
        }
    }
    unregister_script_dispatcher();
    bindings_.clear();
}

auto FunctionHookRegistry::empty() const noexcept -> bool {
    return bindings_.empty();
}

auto FunctionHookRegistry::ensure_script_dispatcher_registered() -> bool {
    if (scriptPreCallbackId_ != Hook::ERROR_ID && scriptPostCallbackId_ != Hook::ERROR_ID) {
        return true;
    }

    const Hook::FCallbackOptions preOptions{
        .bOnce = false,
        .bReadonly = true,
        .OwnerModName = STR("PalworldEditor"),
        .HookName = callbackNamePrefix_ + STR("Pre"),
    };
    scriptPreCallbackId_ = Hook::RegisterProcessLocalScriptFunctionPreCallback(
        [this](Hook::TCallbackIterationData<void>&, UObject* context, FFrame& stack, void* result) {
            dispatch_script(true, context, stack, result);
        },
        preOptions);

    const Hook::FCallbackOptions postOptions{
        .bOnce = false,
        .bReadonly = true,
        .OwnerModName = STR("PalworldEditor"),
        .HookName = callbackNamePrefix_ + STR("Post"),
    };
    scriptPostCallbackId_ = Hook::RegisterProcessLocalScriptFunctionPostCallback(
        [this](Hook::TCallbackIterationData<void>&, UObject* context, FFrame& stack, void* result) {
            dispatch_script(false, context, stack, result);
        },
        postOptions);

    if (scriptPreCallbackId_ != Hook::ERROR_ID && scriptPostCallbackId_ != Hook::ERROR_ID) {
        return true;
    }
    unregister_script_dispatcher();
    return false;
}

auto FunctionHookRegistry::unregister_script_dispatcher() -> void {
    if (scriptPreCallbackId_ != Hook::ERROR_ID) {
        if (!Hook::UnregisterCallback(scriptPreCallbackId_)) {
            Output::send<LogLevel::Warning>(
                STR("PalworldEditor: failed to unregister script pre-hook callback\n"));
        }
        scriptPreCallbackId_ = Hook::ERROR_ID;
    }
    if (scriptPostCallbackId_ != Hook::ERROR_ID) {
        if (!Hook::UnregisterCallback(scriptPostCallbackId_)) {
            Output::send<LogLevel::Warning>(
                STR("PalworldEditor: failed to unregister script post-hook callback\n"));
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
        Output::send<LogLevel::Warning>(
            STR("PalworldEditor: UFunction hook callback rejected an exception\n"));
    } catch (...) {
        Output::send<LogLevel::Warning>(
            STR("PalworldEditor: UFunction hook callback rejected an unknown exception\n"));
    }
}

}  // namespace pal_game
