/**
 * @file dllmain.cpp
 * @brief 实现 PalworldEditor mod 生命周期、ImGui 界面、跨线程请求交接和 DLL 导出入口。
 * @details ImGui 回调运行在 GUI 线程，只读取互斥量保护的快照并提交请求；EngineTick
 *          回调运行在游戏线程，是执行 Unreal 反射操作的唯一入口。结果通过互斥量保护的缓存和
 *          技能快照返回 GUI。构建使用 `cmake --preset ninja-msvc-x64`，部署使用
 *          `cmake --build --preset ninja-msvc-x64 --target deploy`。
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <DynamicOutput/DynamicOutput.hpp>
#include <GUI/GUITab.hpp>
#include <UE4SSProgram.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/Property/FEnumProperty.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UnrealInitializer.hpp>
#include <Windows.h>
#include <common/text_encoding.hpp>
#include <mod/mod_core.hpp>
#include <pal_revive/pal_revive.hpp>
#include <skills/pal_resolution_scheduler.hpp>

using namespace RC;
using namespace RC::Unreal;

namespace {
template <auto Level, typename... Args>
auto log_noexcept(const TCHAR* format, Args&&... args) noexcept -> void {
    try {
        Output::send<Level>(format, std::forward<Args>(args)...);
    } catch (...) {
        // 日志设备在关停期间可能已经销毁；安全路径不能因诊断失败再次抛出。
        static_cast<void>(0);
    }
}

/**
 * @brief 保持本 DLL 代码页存活到进程退出，覆盖 UE4SS 延迟回收失效回调闭包的窗口。
 * @details UE4SS 的全局回调注销会等待在途执行，但无效闭包由独立 GC 线程稍后销毁；
 *          CppMod 随后立即 FreeLibrary。固定模块不保留 mod 实例或 Hook，只防止闭包析构代码失效。
 */
[[nodiscard]] auto pin_current_module() noexcept -> bool {
    HMODULE module{};
    return GetModuleHandleExW(
               GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
               reinterpret_cast<LPCWSTR>(&pin_current_module), &module) != FALSE;
}

/** @brief 卸载时等待游戏线程完成清理的期限；正常热重载只需一个 EngineTick。 */
constexpr auto kUnloadCleanupTimeout = std::chrono::seconds{10};
// 有界重试预算（首次立即 + 最多 kMaximumAttempts-1 次间隔重试）必须完整落在等待窗口内，
// 否则最后一次重试会发生在卸载线程已超时放弃之后，等待语义与重试日程悄然脱节。
static_assert(std::chrono::duration<float>(
                  mod_lifecycle::UnloadCleanupScheduler::kRetryIntervalSeconds* static_cast<float>(
                      mod_lifecycle::UnloadCleanupScheduler::kMaximumAttempts - 1)) <
                  std::chrono::duration<float>(kUnloadCleanupTimeout),
              "bounded unload retry budget must stay inside the unload wait window");
}  // namespace

PalworldEditorMod::PalworldEditorMod() : CppUserModBase() {
    ModName = STR("PalworldEditor");
    ModVersion = STR("1.7.0");
    ModDescription = STR("Item, Pal skill, and same-guild base resource editor for Palworld 1.0");
    ModAuthors = STR("with-fair-wind");

    if (!pin_current_module()) {
        log_noexcept<LogLevel::Error>(
            STR("PalworldEditor: failed to pin the mod DLL; hot unload callback retirement is "
                "not safe.\n"));
        runtimeSafetyDisabled_.store(true, std::memory_order_release);
    }

    Output::send<LogLevel::Verbose>(STR("PalworldEditor loaded (v1.7.0)\n"));

    register_tab(STR("PalworldEditor"), [](CppUserModBase* mod) {
        UE4SS_ENABLE_IMGUI()
        auto* self = static_cast<PalworldEditorMod*>(mod);
        if (self->unloadRequested_.load(std::memory_order_acquire)) {
            return;
        }
        render_main_window(self);
    });
}

PalworldEditorMod::~PalworldEditorMod() {
    // 卸载清理由 uninstall_mod() 在删除前等待游戏线程完成（超时则放弃销毁实例），
    // 因此这里只注销全局回调；业务 Hook 与可逆覆盖已由游戏线程恢复。
    // 只在确实发起过卸载请求时才把未完成当作错误——进程退出直接析构不是卸载路径。
    if (unloadRequested_.load(std::memory_order_acquire)) {
        bool cleanup_succeeded{};
        {
            const std::lock_guard lock(unloadMutex_);
            cleanup_succeeded =
                unload_cleanup_scheduler_.phase() == mod_lifecycle::UnloadCleanupPhase::succeeded;
        }
        if (!cleanup_succeeded) {
            // 此路径对应卸载已判定失败或超时后保留的实例随进程退出析构，
            // 卸载请求已由 uninstall_mod() 处理并得出结论；游戏状态可能未完全恢复。
            log_noexcept<LogLevel::Error>(
                STR("PalworldEditor: 析构时卸载清理未成功（判定失败或超时）；实例保留至进程"
                    "退出，游戏状态可能未完全恢复。\n"));
        }
    }
    runtimeCallbackGate_->deactivate_and_wait();
    unregister_callback(engineTickCallbackId_);
    unregister_callback(loadMapPostCallbackId_);
    unregister_callback(loadMapPreCallbackId_);
}

auto PalworldEditorMod::request_unload_cleanup() -> void {
    unloadRequested_.store(true, std::memory_order_release);
    if (IsGameThreadInitialized() && IsInGameThread()) {
        attempt_unload_cleanup_on_game_thread(0.0F);
    }
}

auto PalworldEditorMod::wait_for_unload_cleanup(const std::chrono::milliseconds timeout)
    -> mod_lifecycle::UnloadCleanupWaitResult {
    using mod_lifecycle::UnloadCleanupPhase;
    using mod_lifecycle::UnloadCleanupWaitResult;
    std::unique_lock lock(unloadMutex_);
    if (engineTickCallbackId_ == Hook::ERROR_ID) {
        // 回调从未注册（on_unreal_init 未执行）：没有需要游戏线程执行的清理。
        unload_cleanup_scheduler_.mark_not_required();
        return unload_cleanup_scheduler_.phase() == UnloadCleanupPhase::succeeded
                   ? UnloadCleanupWaitResult::cleanupSucceeded
                   : UnloadCleanupWaitResult::cleanupFailed;
    }
    const bool decided = unloadCondition_.wait_for(lock, timeout, [this] {
        return unload_cleanup_scheduler_.phase() == UnloadCleanupPhase::succeeded ||
               unload_cleanup_scheduler_.phase() == UnloadCleanupPhase::failed ||
               unload_cleanup_scheduler_.destruction_blocked();
    });
    if (!decided) {
        return UnloadCleanupWaitResult::timedOut;
    }
    // 永久失败锁存或尝试次数耗尽都是"已判定失败"：一旦锁存就不得被后续任何状态覆盖，
    // 销毁判定以它为先；瞬态耗尽也归失败（实例保留），而不是按 succeeded 误删实例。
    if (unload_cleanup_scheduler_.destruction_blocked() ||
        unload_cleanup_scheduler_.phase() == UnloadCleanupPhase::failed) {
        return UnloadCleanupWaitResult::cleanupFailed;
    }
    return UnloadCleanupWaitResult::cleanupSucceeded;
}

auto PalworldEditorMod::unload_failure_is_permanent() -> bool {
    const std::lock_guard lock(unloadMutex_);
    return unload_cleanup_scheduler_.destruction_blocked();
}

auto PalworldEditorMod::on_unreal_init() -> void {
    if (runtimeSafetyDisabled_.load(std::memory_order_acquire)) {
        skillRuntimeSnapshot_.lastResult =
            "Mod DLL 无法固定到进程生命周期；为避免延迟回调访问已卸载代码，运行时功能未启动。";
        skillSnapshotDirty_ = true;
        publish_skill_snapshot_if_dirty();
        return;
    }

    const auto fail_initialization = [this](const TCHAR* logMessage,
                                            const std::string_view userMessage) noexcept {
        log_noexcept<LogLevel::Error>(logMessage);
        runtimeSafetyDisabled_.store(true, std::memory_order_release);
        shutdown_runtime_on_game_thread("运行时初始化失败");
        runtimeCallbackGate_->deactivate_and_wait();
        unregister_callback(engineTickCallbackId_);
        unregister_callback(loadMapPostCallbackId_);
        unregister_callback(loadMapPreCallbackId_);
        worldLifecycleCallbacksReady_.store(false, std::memory_order_release);
        try {
            skillQueue_.clear();
            {
                const std::scoped_lock lock(selectionRequestMutex_);
                selectCurrentPalRequest_.reset();
            }
            skillRuntimeSnapshot_.lastResult = userMessage;
            skillSnapshotDirty_ = true;
            publish_skill_snapshot_if_dirty();
        } catch (...) {
            // 运行时已经停用且 Hook 已钝化；低内存下允许只缺失 GUI 诊断文本。
            static_cast<void>(0);
        }
    };

    const auto callbackGate = runtimeCallbackGate_;
    try {
        const Hook::FCallbackOptions loadMapPreOptions{
            .bOnce = false,
            .bReadonly = true,
            .OwnerModName = STR("PalworldEditor"),
            .HookName = STR("WorldTransitionBegin"),
        };
        {
            const std::lock_guard lock(unloadMutex_);
            loadMapPreCallbackId_ = Hook::RegisterLoadMapPreCallback(
                [this, callbackGate](Hook::TCallbackIterationData<bool>&, UEngine*, FWorldContext&,
                                     FURL, UPendingNetGame*, FString&) {
                    if (!callbackGate->try_enter()) {
                        return;
                    }
                    const RuntimeCallbackLease lease{*callbackGate};
                    if (!unloadRequested_.load(std::memory_order_acquire) &&
                        !runtimeSafetyDisabled_.load(std::memory_order_acquire)) {
                        try {
                            begin_world_transition();
                        } catch (...) {
                            log_noexcept<LogLevel::Error>(STR(
                                "PalworldEditor: LoadMap pre-callback threw; runtime disabled.\n"));
                            runtimeSafetyDisabled_.store(true, std::memory_order_release);
                            shutdown_runtime_on_game_thread("LoadMap 前置回调异常");
                        }
                    }
                },
                loadMapPreOptions);
        }

        const Hook::FCallbackOptions loadMapPostOptions{
            .bOnce = false,
            .bReadonly = true,
            .OwnerModName = STR("PalworldEditor"),
            .HookName = STR("WorldTransitionFinish"),
        };
        {
            const std::lock_guard lock(unloadMutex_);
            loadMapPostCallbackId_ = Hook::RegisterLoadMapPostCallback(
                [this, callbackGate](Hook::TCallbackIterationData<bool>&, UEngine*, FWorldContext&,
                                     FURL, UPendingNetGame*, FString&) {
                    if (!callbackGate->try_enter()) {
                        return;
                    }
                    const RuntimeCallbackLease lease{*callbackGate};
                    if (!unloadRequested_.load(std::memory_order_acquire) &&
                        !runtimeSafetyDisabled_.load(std::memory_order_acquire)) {
                        try {
                            finish_world_transition();
                        } catch (...) {
                            log_noexcept<LogLevel::Error>(
                                STR("PalworldEditor: LoadMap post-callback threw; runtime "
                                    "disabled.\n"));
                            runtimeSafetyDisabled_.store(true, std::memory_order_release);
                            shutdown_runtime_on_game_thread("LoadMap 后置回调异常");
                        }
                    }
                },
                loadMapPostOptions);
        }

        const Hook::FCallbackOptions engineTickOptions{
            .bOnce = false,
            .bReadonly = true,
            .OwnerModName = STR("PalworldEditor"),
            .HookName = STR("GameThreadTick"),
        };
        {
            const std::lock_guard lock(unloadMutex_);
            engineTickCallbackId_ = Hook::RegisterEngineTickPreCallback(
                [this, callbackGate](Hook::TCallbackIterationData<void>&, UEngine*,
                                     const float deltaSeconds, bool) {
                    if (!callbackGate->try_enter()) {
                        return;
                    }
                    const RuntimeCallbackLease lease{*callbackGate};
                    game_thread_tick(deltaSeconds);
                },
                engineTickOptions);
        }
    } catch (...) {
        log_noexcept<LogLevel::Error>(
            STR("PalworldEditor: registering required lifecycle callbacks threw an exception.\n"));
    }

    worldLifecycleCallbacksReady_.store(loadMapPreCallbackId_ != Hook::ERROR_ID &&
                                        loadMapPostCallbackId_ != Hook::ERROR_ID);

    if (engineTickCallbackId_ == Hook::ERROR_ID || !worldLifecycleCallbacksReady_.load()) {
        fail_initialization(
            STR("PalworldEditor: required EngineTick/LoadMap callbacks could not be registered; "
                "runtime features are disabled.\n"),
            "UE4SS 游戏线程/世界切换回调注册失败；为避免跨世界访问，运行时功能已停用。");
        return;
    }

    try {
        // 远程终端配置：mods/<ModName>/remote_palbox.ini（缺失时回退默认值）。
        const auto modsDirectory = UE4SSProgram::get_program().get_mods_directory();
        const auto iniPath = (std::filesystem::path(modsDirectory) /
                              std::filesystem::path(ModName) / L"remote_palbox.ini")
                                 .wstring();
        remotePalboxRuntime_.load_config(text_encoding::to_utf8(iniPath));
        // 标记点传送配置：mods/<ModName>/waypoint_teleport.ini（缺失时回退默认值）。
        const auto waypointIniPath = (std::filesystem::path(modsDirectory) /
                                      std::filesystem::path(ModName) / L"waypoint_teleport.ini")
                                         .wstring();
        waypointTeleportRuntime_.load_config(text_encoding::to_utf8(waypointIniPath));

        wantProbeObject_.store(true);
        want_scan_items_.store(true);
        itemCatalogScanScheduler_.begin_world(worldSession_.generation());
        static_cast<void>(stack_limit_ledger_.begin_world(worldSession_.generation()));
        stack_limit_phase_.store(stack_limit_ledger_.phase(worldSession_.generation()),
                                 std::memory_order_release);
        static_cast<void>(reviveTimerLedger_.begin_world(worldSession_.generation()));
        reviveTimerPhase_.store(reviveTimerLedger_.phase(worldSession_.generation()),
                                std::memory_order_release);
        baseResourceBridge_.on_world_ready(worldSession_.generation());
        static_cast<void>(grappleLedger_.begin_world(worldSession_.generation()));
        grappleReadinessScheduler_.begin_world(worldSession_.generation());
        grappleRuntimePhase_.store(grappleLedger_.phase(worldSession_.generation()),
                                   std::memory_order_release);
        captureRuntime_.on_world_begin();
        captureRuntimePhase_.store(captureRuntime_.phase(), std::memory_order_release);
    } catch (...) {
        fail_initialization(
            STR("PalworldEditor: runtime initialization threw after callback registration.\n"),
            "运行时初始化异常；已在游戏线程清理并停用全部功能。");
    }
}

auto PalworldEditorMod::on_update() -> void {}

auto PalworldEditorMod::game_thread_tick(const float deltaSeconds) -> void {
    if (unloadRequested_.load(std::memory_order_acquire)) {
        // 失败时低频、有限重试；耗尽后只保留实例，不再执行反射。
        attempt_unload_cleanup_on_game_thread(deltaSeconds);
        return;
    }
    if (runtimeSafetyDisabled_.load(std::memory_order_acquire) ||
        !worldSession_.can_access_unreal()) {
        return;
    }

    try {
        process_runtime_services(deltaSeconds);
        const auto worldContextReady = process_inventory_requests(deltaSeconds);
        process_stack_limit_work(worldContextReady);
        process_revive_timer_work(worldContextReady);
        process_fishing_boost_work(worldContextReady);
        process_pal_edit_requests();
        process_initialization_tasks();
        process_utility_requests();
        publish_runtime_state();
    } catch (const std::exception&) {
        log_noexcept<LogLevel::Error>(
            STR("PalworldEditor: EngineTick failed with a standard exception; disabling runtime "
                "safely.\n"));
        runtimeSafetyDisabled_.store(true, std::memory_order_release);
        shutdown_runtime_on_game_thread("EngineTick 异常");
    } catch (...) {
        log_noexcept<LogLevel::Error>(
            STR("PalworldEditor: EngineTick failed with a non-standard exception; disabling "
                "runtime safely.\n"));
        runtimeSafetyDisabled_.store(true, std::memory_order_release);
        shutdown_runtime_on_game_thread("EngineTick 异常");
    }
}

auto PalworldEditorMod::shutdown_runtime_on_game_thread(const std::string_view reason) noexcept
    -> mod_lifecycle::CleanupOutcome {
    using mod_lifecycle::CleanupOutcome;
    if (!IsGameThreadInitialized() || !IsInGameThread()) {
        return CleanupOutcome::transientFailure;
    }

    CleanupOutcome worst = CleanupOutcome::succeeded;
    const auto run_cleanup = [&worst](const auto& cleanup, const TCHAR* failure) noexcept {
        try {
            const CleanupOutcome outcome = cleanup();
            if (outcome != CleanupOutcome::succeeded) {
                worst = mod_lifecycle::worse_outcome(worst, outcome);
                log_noexcept<LogLevel::Error>(failure);
            }
        } catch (...) {
            // 反射路径异常按瞬态处理：账本仍持有恢复责任，交给有界重试。
            worst = mod_lifecycle::worse_outcome(worst, CleanupOutcome::transientFailure);
            log_noexcept<LogLevel::Error>(failure);
        }
    };

    requestedCaptureUnlock_.store(false, std::memory_order_release);
    requestedBaseSharingEnabled_.store(false, std::memory_order_release);
    requestedGrappleNoCooldown_.store(false, std::memory_order_release);
    requested_stack_unlimited_.store(false, std::memory_order_release);
    captureSettingDirty_.store(false, std::memory_order_release);
    baseSharingSettingDirty_.store(false, std::memory_order_release);
    grappleSettingDirty_.store(false, std::memory_order_release);
    stack_setting_dirty_.store(false, std::memory_order_release);

    // 捕获事务恢复失败是永久性的：pending 事务在失败时已被丢弃，锁存后任何重试都
    // 无法挽回，因此映射为 permanentFailure 让等待线程立即得到失败结论。仅剩 Hook
    // 注销残留时为瞬态：失败绑定保留在登记器中，按卸载重试日程再次尝试。
    run_cleanup(
        [this] {
            const bool clean = captureRuntime_.shutdown();
            if (captureRuntime_.shutdown_restore_failed()) {
                return CleanupOutcome::permanentFailure;
            }
            return clean ? CleanupOutcome::succeeded : CleanupOutcome::transientFailure;
        },
        STR("PalworldEditor: capture overrides could not be restored during shutdown.\n"));
    run_cleanup(
        [this] {
            return baseResourceBridge_.shutdown_hooks() ? CleanupOutcome::succeeded
                                                        : CleanupOutcome::transientFailure;
        },
        STR("PalworldEditor: resource hooks could not be removed during shutdown.\n"));
    run_cleanup(
        [this, reason] {
            grappleLedger_.set_desired(false);
            return restore_grapple_overrides(reason) ? CleanupOutcome::succeeded
                                                     : CleanupOutcome::transientFailure;
        },
        STR("PalworldEditor: grapple values could not be restored during shutdown.\n"));
    run_cleanup(
        [this, reason] {
            stack_limit_ledger_.set_desired(false);
            return restore_stack_limit_overrides(reason) ? CleanupOutcome::succeeded
                                                         : CleanupOutcome::transientFailure;
        },
        STR("PalworldEditor: stack limits could not be restored during shutdown.\n"));
    run_cleanup(
        [this, reason] {
            reviveTimerLedger_.set_desired(false);
            return restore_revive_timer_overrides(reason) ? CleanupOutcome::succeeded
                                                          : CleanupOutcome::transientFailure;
        },
        STR("PalworldEditor: revive timer values could not be restored during shutdown.\n"));
    run_cleanup(
        [this] {
            fishingBoostLedger_.set_desired(false);
            // 目标不可解析（世界已退出、对象已销毁）等于无需恢复：新世界实例使用
            // 原生值；其余失败按瞬态清理重试。
            const auto status = fishing_boost::restore(
                fishingBoostLedger_, UObjectGlobals::FindFirstOf(pal_game::kInventoryClassName));
            return status == fishing_boost::GatewayStatus::succeeded ||
                           status == fishing_boost::GatewayStatus::targetUnavailable
                       ? CleanupOutcome::succeeded
                       : CleanupOutcome::transientFailure;
        },
        STR("PalworldEditor: fishing values could not be restored during shutdown.\n"));
    worldInitializationReady_ = false;
    return worst;
}

auto PalworldEditorMod::attempt_unload_cleanup_on_game_thread(const float delta_seconds) noexcept
    -> void {
    {
        const std::lock_guard lock(unloadMutex_);
        if (!unload_cleanup_scheduler_.advance(delta_seconds)) {
            return;
        }
    }

    const auto outcome = shutdown_runtime_on_game_thread("卸载");
    bool should_notify = false;
    {
        const std::lock_guard lock(unloadMutex_);
        const bool blocked_before = unload_cleanup_scheduler_.destruction_blocked();
        unload_cleanup_scheduler_.complete(outcome);
        // 成功、进入失败终态（尝试耗尽）或新近判定不可恢复失败时立即释放等待线程；
        // 纯瞬态失败继续按日程重试，等待线程保持等待直到得出结论或超时。
        should_notify =
            unload_cleanup_scheduler_.phase() == mod_lifecycle::UnloadCleanupPhase::succeeded ||
            unload_cleanup_scheduler_.phase() == mod_lifecycle::UnloadCleanupPhase::failed ||
            (!blocked_before && unload_cleanup_scheduler_.destruction_blocked());
    }
    if (should_notify) {
        unloadCondition_.notify_all();
    }
}

auto PalworldEditorMod::process_runtime_services(const float deltaSeconds) -> void {
    if (baseSharingSettingDirty_.exchange(false)) {
        baseResourceBridge_.set_enabled(requestedBaseSharingEnabled_.load());
    }
    if (grappleSettingDirty_.exchange(false)) {
        const auto desired = requestedGrappleNoCooldown_.load(std::memory_order_acquire);
        grappleLedger_.set_desired(desired);
        if (desired) {
            grappleReadinessScheduler_.request(worldSession_.generation());
        }
    }
    if (grappleRetryRequested_.exchange(false, std::memory_order_acq_rel) &&
        grappleLedger_.request_retry(worldSession_.generation())) {
        grappleReadinessScheduler_.request(worldSession_.generation());
    }
    if (captureSettingDirty_.exchange(false)) {
        captureRuntime_.set_config(
            {.unlockUncapturable = requestedCaptureUnlock_.load(std::memory_order_acquire),
             .forceHundredPercent = requestedCaptureForcePercent_.load(std::memory_order_acquire)});
    }
    captureRuntime_.tick();
    captureRuntimePhase_.store(captureRuntime_.phase(), std::memory_order_release);
    process_grapple_work(deltaSeconds);
    baseResourceBridge_.ensure_hooks_registered();
    baseResourceBridge_.tick(deltaSeconds);
    remotePalboxRuntime_.tick(deltaSeconds, worldSession_);
    waypointTeleportRuntime_.tick(deltaSeconds, worldSession_);

    if (wantProbeObject_.exchange(false)) {
        if (const auto object = UObjectGlobals::StaticFindObject<UObject*>(
                nullptr, nullptr, STR("/Script/CoreUObject.Object"))) {
            Output::send<LogLevel::Verbose>(STR("Object Name: {}\n"), object->GetFullName());
        }
    }
}

auto PalworldEditorMod::process_inventory_requests(const float deltaSeconds) -> bool {
    // Give items
    std::string item;
    int count = 0;
    bool doGive = false;
    {
        const std::lock_guard lock(req_mutex_);
        if (give_requested_.load()) {
            give_requested_.store(false);
            item = give_item_;
            count = give_count_;
            doGive = true;
        }
    }
    // Modify inventory count
    int32_t modSlot = 0;
    int32_t modCount = 0;
    bool doMod = false;
    {
        const std::lock_guard lock(req_mutex_);
        if (modify_requested_.load()) {
            modify_requested_.store(false);
            modSlot = modify_slot_;
            modCount = modify_count_;
            doMod = true;
        }
    }

    const bool scanRequested = want_scan_items_.exchange(false, std::memory_order_acq_rel);
    if (!worldInitializationReady_ && (doGive || doMod)) {
        worldInitializationReady_ = pal_game::is_valid(pal_game::get_main_container());
    }

    // StaticItemDataMap may become ready after LoadMap. The existing bounded scheduler also gates
    // Common-container readiness checks, so startup retry never becomes a per-frame FindFirstOf.
    const auto worldGeneration = worldSession_.generation();
    if (scanRequested) {
        itemCatalogScanScheduler_.request(worldGeneration);
    }
    if (itemCatalogScanScheduler_.advance(deltaSeconds, worldGeneration, true)) {
        if (!worldInitializationReady_) {
            worldInitializationReady_ = pal_game::is_valid(pal_game::get_main_container());
        }
        if (worldInitializationReady_) {
            auto result = pal_game::scan_all_items();
            static_cast<void>(
                itemCatalogScanScheduler_.complete(worldGeneration, result.usedStaticItemDataMap));
            const std::lock_guard lock(inv_mutex_);
            if (result.usedStaticItemDataMap || item_db_cache_.items.empty()) {
                item_db_cache_ = std::move(result.catalog);
            }
        } else {
            static_cast<void>(itemCatalogScanScheduler_.complete(worldGeneration, false));
        }
    }

    if (doGive && worldInitializationReady_) {
        pal_game::give_items(item, static_cast<int32>(count));
        want_read_.store(true);
    }
    if (doMod && worldInitializationReady_ && !inventoryWritesDisabled_.load()) {
        const auto result = pal_game::set_slot_count(modSlot, modCount);
        if (result == pal_game::SlotCountWriteStatus::rollbackFailed) {
            inventoryWritesDisabled_.store(true, std::memory_order_release);
        }
        want_read_.store(true);
    }

    // Read inventory
    if (worldInitializationReady_ && want_read_.exchange(false, std::memory_order_acq_rel)) {
        auto fresh = pal_game::read_inventory();
        const std::lock_guard lock(inv_mutex_);
        if (selected_ >= static_cast<int>(fresh.size())) {
            selected_ = -1;
        }
        inv_cache_ = std::move(fresh);
    }

    return worldInitializationReady_;
}

auto PalworldEditorMod::process_pal_edit_requests() -> void {
    std::optional<skill_editor::WorldBoundRequest> selectionRequest;
    {
        const std::lock_guard lock(selectionRequestMutex_);
        selectionRequest = std::exchange(selectCurrentPalRequest_, std::nullopt);
    }
    const bool selectionRequested =
        selectionRequest.has_value() &&
        skill_editor::request_can_run(*selectionRequest, worldSession_) &&
        worldLifecycleCallbacksReady_.load();

    std::optional<skill_editor::SkillEditRequest> editRequest;
    std::optional<pal_stats::PalStatEditRequest> statRequest;
    std::optional<pal_identity::PalIdentityEditRequest> identityRequest;
    if (selectionRequested) {
        skillQueue_.clear();
        statRequestSlot_.clear();
        identityRequestSlot_.clear();
    } else {
        editRequest = skillQueue_.try_pop();
        statRequest = statRequestSlot_.consume();
        identityRequest = identityRequestSlot_.consume();
    }

    const auto trigger = skill_editor::decide_pal_resolution(
        selectionRequested,
        editRequest.has_value() || statRequest.has_value() || identityRequest.has_value());
    std::optional<pal_game::SelectedPalTarget> resolvedPal;
    if (trigger != skill_editor::PalResolutionTrigger::none) {
        resolvedPal = pal_game::resolve_selected_otomo();
        const bool resolved =
            resolvedPal->status == skill_editor::SelectedTargetResolutionStatus::success &&
            resolvedPal->observation.is_valid() && pal_game::is_valid(resolvedPal->parameter);
        const skill_editor::TargetResolutionSnapshot nextResolution{
            .resolved = resolved,
            .observation =
                resolved ? resolvedPal->observation : skill_editor::SelectedTargetObservation{},
            .status = resolvedPal->status,
            .holderCandidateCount = resolvedPal->holderCandidateCount,
            .localHolderCandidateCount = resolvedPal->localHolderCandidateCount,
            .holderCandidateClasses = resolvedPal->holderCandidateClasses,
        };
        skillSnapshotDirty_ = targetResolutionState_.update(nextResolution) || skillSnapshotDirty_;

        if (!lastResolutionStatus_.has_value() || *lastResolutionStatus_ != resolvedPal->status) {
            Output::send<LogLevel::Warning>(
                STR("PalworldEditor: selected Pal resolution status={}, "
                    "holder_candidates={}, local_candidates={}, classes=[{}]\n"),
                static_cast<int32>(resolvedPal->status),
                static_cast<int32>(resolvedPal->holderCandidateCount),
                static_cast<int32>(resolvedPal->localHolderCandidateCount),
                resolvedPal->holderCandidateClasses);
            lastResolutionStatus_ = resolvedPal->status;
        }
    }

    const auto& resolution = targetResolutionState_.current();
    if (selectionRequested) {
        if (resolvedPal.has_value() && resolution.resolved &&
            selectedTarget_.confirm(resolution.observation) && worldSession_.confirm_target()) {
            auto skillRead = skillGateway_.read_state(
                reinterpret_cast<skill_editor::SkillTarget>(resolvedPal->parameter));
            skillRuntimeSnapshot_.state = std::move(skillRead.state);
            skillRuntimeSnapshot_.palStat = statGateway_.read_stats(
                reinterpret_cast<pal_stats::PalStatTarget>(resolvedPal->parameter));
            skillRuntimeSnapshot_.palIdentity = identityGateway_.read_identity(
                reinterpret_cast<pal_identity::PalIdentityTarget>(resolvedPal->parameter),
                resolvedPal->spawnStateKnown, resolvedPal->selectedIsSpawned);
            if (!skillRead.passiveReadable || !skillRead.activeReadable) {
                skillRuntimeSnapshot_.lastResult =
                    "技能读取不完整：当前不可安全编辑未成功读取的技能分区。";
            } else {
                skillRuntimeSnapshot_.lastResult.clear();
            }
        } else {
            skillRuntimeSnapshot_.palStat = {};
            skillRuntimeSnapshot_.palIdentity = {};
            const auto reason = skill_editor::resolution_status_message(resolution.status);
            skillRuntimeSnapshot_.lastResult = "选择失败：";
            skillRuntimeSnapshot_.lastResult.append(reason.data(), reason.size());
        }
        skillSnapshotDirty_ = true;
    }

    std::optional<skill_editor::SkillEditResult> editResult;
    if (editRequest.has_value()) {
        const auto target =
            resolvedPal.has_value() && resolution.resolved
                ? reinterpret_cast<skill_editor::SkillTarget>(resolvedPal->parameter)
                : skill_editor::SkillTarget{};
        editResult = skill_editor::apply_if_target_is_current(
            *editRequest, selectedTarget_, resolution.observation, target, worldSession_,
            [this](const skill_editor::SkillEditRequest& executableRequest) {
                return skill_editor::execute_skill_edit(skillGateway_, executableRequest);
            });
        if (!editResult.has_value()) {
            skillQueue_.clear();
            editResult = skill_editor::SkillEditResult{
                .status = skill_editor::SkillEditStatus::rejected,
                .message = "当前高亮帕鲁与已选择目标不一致或暂时无法确认；本次修改未执行。",
            };
        } else {
            skillRuntimeSnapshot_.state = editResult->state;
        }
        skillRuntimeSnapshot_.lastResult = editResult->message;
        skillSnapshotDirty_ = true;
    }

    if (statRequest.has_value()) {
        const auto target = resolvedPal.has_value() && resolution.resolved
                                ? reinterpret_cast<pal_stats::PalStatTarget>(resolvedPal->parameter)
                                : pal_stats::PalStatTarget{};
        const bool targetCurrent = skill_editor::bound_target_request_is_current(
            *statRequest, selectedTarget_, resolution.observation, target, worldSession_);
        const bool hasCoreChange = pal_stats::has_core_stat_change(statRequest->values);
        const bool hasWorkChange = pal_stats::has_work_suitability_change(statRequest->values);
        const bool safetyDisabled = (hasCoreChange && statWritesDisabledForWorld_) ||
                                    (hasWorkChange && workSuitabilityWritesDisabledForWorld_);
        if (targetCurrent && !safetyDisabled && pal_stats::has_any_change(statRequest->values) &&
            statGateway_.is_valid(target)) {
            const auto result = statGateway_.apply_stat_edit(target, *statRequest);
            skillRuntimeSnapshot_.palStat = result.snapshot;
            skillRuntimeSnapshot_.lastResult = result.message;
            if (result.status == pal_stats::PalStatEditStatus::rollbackFailed) {
                statWritesDisabledForWorld_ = hasCoreChange || statWritesDisabledForWorld_;
                workSuitabilityWritesDisabledForWorld_ =
                    hasWorkChange || workSuitabilityWritesDisabledForWorld_;
            }
        } else {
            statRequestSlot_.clear();
            skillRuntimeSnapshot_.lastResult =
                safetyDisabled ? "本世界对应属性域曾发生恢复验证失败；该类写入已安全停用。"
                               : "当前高亮帕鲁与已选择目标不一致或暂时无法确认；"
                                 "属性修改未执行。";
        }
        skillSnapshotDirty_ = true;
    }

    if (identityRequest.has_value()) {
        const auto target =
            resolvedPal.has_value() && resolution.resolved
                ? reinterpret_cast<pal_identity::PalIdentityTarget>(resolvedPal->parameter)
                : pal_identity::PalIdentityTarget{};
        const bool spawnStateKnown =
            resolvedPal.has_value() && resolution.resolved && resolvedPal->spawnStateKnown;
        const bool selectedIsSpawned = spawnStateKnown && resolvedPal->selectedIsSpawned;
        const bool targetCurrent = skill_editor::bound_target_request_is_current(
            *identityRequest, selectedTarget_, resolution.observation, target, worldSession_);
        if (targetCurrent && spawnStateKnown && !identityWritesDisabledForWorld_ &&
            pal_identity::has_any_change(identityRequest->values)) {
            const auto result = identityGateway_.apply_identity_edit(
                target, spawnStateKnown, selectedIsSpawned, *identityRequest);
            skillRuntimeSnapshot_.palIdentity = result.snapshot;
            skillRuntimeSnapshot_.lastResult = result.message;
            if (result.status == pal_identity::PalIdentityEditStatus::rollbackFailed) {
                identityWritesDisabledForWorld_ = true;
            }
        } else {
            identityRequestSlot_.clear();
            skillRuntimeSnapshot_.lastResult =
                identityWritesDisabledForWorld_
                    ? "本世界曾发生形态恢复验证失败；后续 Alpha、Lucky 与觉醒写入已安全停用。"
                    : "当前高亮帕鲁与已选择目标不一致或暂时无法确认；"
                      "形态修改未执行。";
        }
        skillSnapshotDirty_ = true;
    }
}

auto PalworldEditorMod::process_initialization_tasks() -> void {
    const bool manualRefreshRequested = wantRefreshSkillCatalog_.exchange(false);
    const bool catalogReady =
        skill_editor::catalog_is_ready_for_editing(skillRuntimeSnapshot_.catalog);
    const bool refreshRequested = skillCatalogRefreshScheduler_.should_refresh(
        manualRefreshRequested, catalogReady,
        skill_editor::SkillCatalogRefreshScheduler::clock::now(), [this] {
            const auto ready = pal_game::is_valid(pal_game::get_main_container());
            worldInitializationReady_ = worldInitializationReady_ || ready;
            return ready;
        });
    if (refreshRequested) {
        refresh_skill_catalog_on_game_thread();
    }
    advance_passive_classification_on_game_thread();
}

auto PalworldEditorMod::publish_runtime_state() -> void {
    const auto& resolution = targetResolutionState_.current();
    const auto update_runtime_value = [this](auto& current, auto next) {
        if (current != next) {
            current = std::move(next);
            skillSnapshotDirty_ = true;
        }
    };
    update_runtime_value(skillRuntimeSnapshot_.targetGeneration, selectedTarget_.generation());
    update_runtime_value(skillRuntimeSnapshot_.worldGeneration, worldSession_.generation());
    update_runtime_value(skillRuntimeSnapshot_.worldAccessible, worldSession_.can_access_unreal());
    update_runtime_value(skillRuntimeSnapshot_.worldLifecycleCallbacksReady,
                         worldLifecycleCallbacksReady_.load());
    update_runtime_value(skillRuntimeSnapshot_.targetConfirmedForWorld,
                         worldSession_.is_target_confirmed());
    update_runtime_value(skillRuntimeSnapshot_.targetSelected, selectedTarget_.is_selected());
    update_runtime_value(skillRuntimeSnapshot_.targetMatchesCurrent,
                         worldSession_.is_target_confirmed() && resolution.resolved &&
                             selectedTarget_.matches_current(resolution.observation));
    update_runtime_value(skillRuntimeSnapshot_.palName, selectedTarget_.is_selected()
                                                            ? selectedTarget_.current().name
                                                            : std::string{});
    update_runtime_value(skillRuntimeSnapshot_.resolutionStatus, resolution.status);
    update_runtime_value(skillRuntimeSnapshot_.pending, skillQueue_.size() != 0);
    update_runtime_value(skillRuntimeSnapshot_.statWritesDisabled, statWritesDisabledForWorld_);
    update_runtime_value(skillRuntimeSnapshot_.workSuitabilityWritesDisabled,
                         workSuitabilityWritesDisabledForWorld_);
    update_runtime_value(skillRuntimeSnapshot_.identityWritesDisabled,
                         identityWritesDisabledForWorld_);
    if (!selectedTarget_.is_selected() && (!skillRuntimeSnapshot_.state.passiveIds.empty() ||
                                           !skillRuntimeSnapshot_.state.activeSkills.empty())) {
        skillRuntimeSnapshot_.state = {};
        skillSnapshotDirty_ = true;
    }
    publish_skill_snapshot_if_dirty();
}

auto PalworldEditorMod::process_utility_requests() -> void {
    // Revive team pals
    if (wantReviveTeam_.exchange(false)) {
        revive_team_pals();
    }

    // Discover
    if (want_discover_.exchange(false)) {
        pal_game::discover_objects();
    }
}

auto PalworldEditorMod::unregister_callback(Hook::GlobalCallbackId& callbackId) noexcept -> void {
    const std::lock_guard lock(unloadMutex_);
    if (callbackId == Hook::ERROR_ID) {
        return;
    }
    const auto ownedId = callbackId;
    callbackId = Hook::ERROR_ID;
    try {
        if (!Hook::UnregisterCallback(ownedId)) {
            log_noexcept<LogLevel::Warning>(
                STR("PalworldEditor: failed to unregister callback id={}\n"), ownedId);
        }
    } catch (...) {
        log_noexcept<LogLevel::Warning>(
            STR("PalworldEditor: unregistering callback id={} threw an exception.\n"), ownedId);
    }
}

auto PalworldEditorMod::refresh_skill_catalog_on_game_thread() -> void {
    const auto previous = skillRuntimeSnapshot_.catalog;
    auto refreshed = skillGateway_.load_catalog();
    const bool passiveRefreshSucceeded = refreshed.passive.ready;
    skillRuntimeSnapshot_.catalog = skill_editor::with_catalog_fallback(previous, refreshed);

    if (passiveRefreshSucceeded) {
        hadUsablePassiveClassificationBeforeRefresh_ = previous.passiveClassification.ready;
        skill_editor::apply_passive_metadata(skillRuntimeSnapshot_.catalog.passive.skills,
                                             passiveSkillMetadataCache_);
        passiveClassificationJob_.start(skillRuntimeSnapshot_.catalog.passive.skills,
                                        passiveSkillMetadataCache_);
        const auto status = passiveClassificationJob_.status();
        skillRuntimeSnapshot_.catalog.passiveClassification = status;
        passiveClassificationCompleted_.store(status.completed, std::memory_order_relaxed);
        passiveClassificationTotal_.store(status.total, std::memory_order_relaxed);
        passiveClassificationElapsed_ = {};
        passiveClassificationTicks_ = 0;
        if (!passiveClassificationJob_.active()) {
            finish_passive_classification_on_game_thread();
        }
    }
    skillSnapshotDirty_ = true;
}

auto PalworldEditorMod::advance_passive_classification_on_game_thread() -> void {
    if (!passiveClassificationJob_.active()) {
        return;
    }

    const auto ids = passiveClassificationJob_.next_batch(kPassiveMetadataBatchSize);
    const auto batch = skillGateway_.load_passive_skill_metadata_batch(
        ids, kPassiveMetadataBatchSize, kPassiveMetadataBudget);
    passiveClassificationElapsed_ += batch.elapsed;
    ++passiveClassificationTicks_;

    if (!batch.error.empty()) {
        passiveClassificationJob_.fail(batch.error);
        finish_passive_classification_on_game_thread();
        return;
    }
    if (batch.entries.empty()) {
        passiveClassificationJob_.fail("passive metadata batch made no progress");
        finish_passive_classification_on_game_thread();
        return;
    }
    if (!passiveClassificationJob_.complete_batch(batch.entries, passiveSkillMetadataCache_)) {
        finish_passive_classification_on_game_thread();
        return;
    }

    const auto status = passiveClassificationJob_.status();
    passiveClassificationCompleted_.store(status.completed, std::memory_order_relaxed);
    if (!passiveClassificationJob_.active()) {
        finish_passive_classification_on_game_thread();
    }
}

auto PalworldEditorMod::finish_passive_classification_on_game_thread() -> void {
    skill_editor::apply_passive_metadata(skillRuntimeSnapshot_.catalog.passive.skills,
                                         passiveSkillMetadataCache_);
    auto status = skill_editor::with_passive_classification_fallback(
        passiveClassificationJob_.status(), hadUsablePassiveClassificationBeforeRefresh_);
    skillRuntimeSnapshot_.catalog.passiveClassification = status;
    passiveClassificationCompleted_.store(status.completed, std::memory_order_relaxed);
    passiveClassificationTotal_.store(status.total, std::memory_order_relaxed);
    skillSnapshotDirty_ = true;

    const auto known = std::ranges::count_if(
        skillRuntimeSnapshot_.catalog.passive.skills,
        [](const skill_editor::SkillOption& option) { return option.passiveMetadata.has_value(); });
    const auto unknown =
        skillRuntimeSnapshot_.catalog.passive.skills.size() - static_cast<std::size_t>(known);
    if (status.error.empty()) {
        Output::send<LogLevel::Verbose>(
            STR("PalworldEditor: passive classification completed: known={}, "
                "unknown={}, ticks={}, elapsed_us={}\n"),
            known, unknown, passiveClassificationTicks_, passiveClassificationElapsed_.count());
    } else {
        Output::send<LogLevel::Warning>(
            STR("PalworldEditor: passive classification failed after {}/{}: {}; "
                "ticks={}, elapsed_us={}\n"),
            status.completed, status.total, text_encoding::widen_ascii(status.error),
            passiveClassificationTicks_, passiveClassificationElapsed_.count());
    }
}

auto PalworldEditorMod::set_grapple_runtime_status(std::string status) -> void {
    const std::lock_guard lock(grappleStatusMutex_);
    grappleRuntimeStatus_ = std::move(status);
}

auto PalworldEditorMod::revive_team_pals() -> void {
    const auto result = pal_revive::revive_team_pals();
    switch (result.error) {
        case pal_revive::TeamReviveError::holderUnavailable:
            skillRuntimeSnapshot_.lastResult = "复活失败：未找到队伍 Holder。";
            skillSnapshotDirty_ = true;
            return;
        case pal_revive::TeamReviveError::invalidSlotCount:
            skillRuntimeSnapshot_.lastResult = "复活失败：队伍槽位数异常。";
            skillSnapshotDirty_ = true;
            return;
        case pal_revive::TeamReviveError::handleInterfaceUnavailable:
            skillRuntimeSnapshot_.lastResult = "复活失败：队伍槽位接口不可用。";
            skillSnapshotDirty_ = true;
            return;
        case pal_revive::TeamReviveError::none:
            break;
    }

    if (result.rollbackFailed) {
        skillRuntimeSnapshot_.lastResult = "复活失败：写后校验失败且无法完整回滚；本次操作已停止。";
    } else if (result.revivedCount > 0 && result.failedCount > 0) {
        skillRuntimeSnapshot_.lastResult = "复活了 " + std::to_string(result.revivedCount) +
                                           " 只队伍帕鲁，另有 " +
                                           std::to_string(result.failedCount) + " 只未能安全修改。";
    } else if (result.revivedCount > 0) {
        skillRuntimeSnapshot_.lastResult =
            "复活了 " + std::to_string(result.revivedCount) + " 只队伍帕鲁。";
    } else if (result.failedCount > 0) {
        skillRuntimeSnapshot_.lastResult =
            "复活失败：有 " + std::to_string(result.failedCount) + " 只帕鲁无法安全修改。";
    } else {
        skillRuntimeSnapshot_.lastResult = "队伍中没有需要复活的帕鲁。";
    }
    skillSnapshotDirty_ = true;
}

auto PalworldEditorMod::process_stack_limit_work(const bool worldContextReady) -> void {
    if (worldContextReady && stack_setting_dirty_.exchange(false, std::memory_order_acq_rel)) {
        const bool desired = requested_stack_unlimited_.load(std::memory_order_acquire);
        stack_limit_ledger_.set_desired(desired);
        if (!desired && stack_limit_ledger_.records().empty()) {
            publish_stack_limit_status("已取消高堆叠上限请求；当前没有需要恢复的对象。");
        }
    }

    const auto generation = worldSession_.generation();
    switch (stack_limit_ledger_.next_work(generation, worldContextReady)) {
        case item_stack_limit::StackLimitWork::none:
            break;
        case item_stack_limit::StackLimitWork::apply: {
            if (!stack_limit_ledger_.begin_apply(generation)) {
                break;
            }
            stack_limit_phase_.store(item_stack_limit::StackLimitRuntimePhase::applying,
                                     std::memory_order_release);
            auto result = item_stack_limit::apply_stack_limit_override();
            static_cast<void>(stack_limit_ledger_.complete_apply(
                generation, item_stack_limit::to_apply_outcome(result.status),
                std::move(result.records)));
            if (!stack_limit_ledger_.desired()) {
                requested_stack_unlimited_.store(false, std::memory_order_release);
            }
            publish_stack_limit_status(std::move(result.message));
            break;
        }
        case item_stack_limit::StackLimitWork::restore:
            static_cast<void>(restore_stack_limit_overrides("关闭开关"));
            break;
    }

    stack_limit_phase_.store(stack_limit_ledger_.phase(generation), std::memory_order_release);
}

auto PalworldEditorMod::restore_stack_limit_overrides(const std::string_view reason) -> bool {
    stack_limit_ledger_.set_desired(false);
    requested_stack_unlimited_.store(false, std::memory_order_release);
    if (stack_limit_ledger_.records().empty()) {
        stack_limit_phase_.store(stack_limit_ledger_.phase(worldSession_.generation()),
                                 std::memory_order_release);
        return true;
    }

    auto result = item_stack_limit::restore_stack_limit_override(stack_limit_ledger_.records());
    const bool succeeded = result.succeeded();
    const bool ledger_accepted =
        stack_limit_ledger_.complete_restore(succeeded, std::move(result.records));
    std::string message{reason};
    message.append("：");
    message.append(result.message);
    if (!ledger_accepted) {
        message.append(" 反射结果与恢复账本不一致；已保留原账本并安全停用。");
    }
    publish_stack_limit_status(std::move(message));
    stack_limit_phase_.store(stack_limit_ledger_.phase(worldSession_.generation()),
                             std::memory_order_release);
    return succeeded && ledger_accepted;
}

auto PalworldEditorMod::publish_stack_limit_status(std::string message) -> void {
    const std::lock_guard lock(stack_limit_status_mutex_);
    stack_limit_status_ = std::move(message);
}

auto PalworldEditorMod::process_revive_timer_work(const bool worldContextReady) -> void {
    if (worldContextReady && reviveTimerSettingDirty_.exchange(false, std::memory_order_acq_rel)) {
        reviveTimerLedger_.set_desired(requestedReviveTimerRemove_.load(std::memory_order_acquire));
    }
    if (reviveTimerRetryRequested_.exchange(false, std::memory_order_acq_rel)) {
        reviveTimerLedger_.request_retry();
    }

    const auto generation = worldSession_.generation();
    switch (reviveTimerLedger_.next_work(generation, worldContextReady)) {
        case revive_timer::ReviveTimerWork::none:
            break;
        case revive_timer::ReviveTimerWork::apply: {
            if (!reviveTimerLedger_.begin_apply(generation)) {
                break;
            }
            reviveTimerPhase_.store(revive_timer::ReviveTimerRuntimePhase::applying,
                                    std::memory_order_release);
            auto result = revive_timer::apply_revive_timer_override();
            static_cast<void>(reviveTimerLedger_.complete_apply(
                generation, revive_timer::to_apply_outcome(result.status), result.original));
            if (!reviveTimerLedger_.desired()) {
                requestedReviveTimerRemove_.store(false, std::memory_order_release);
            }
            publish_revive_timer_status(std::move(result.message));
            break;
        }
        case revive_timer::ReviveTimerWork::restore:
            static_cast<void>(restore_revive_timer_overrides("关闭开关"));
            break;
    }

    reviveTimerPhase_.store(reviveTimerLedger_.phase(generation), std::memory_order_release);
}

auto PalworldEditorMod::process_fishing_boost_work(const bool worldContextReady) -> void {
    // 目标子系统不可用时的有界重试：开关变化立即尝试一次，此后每 2 秒一次、
    // 连续 15 次仍不可用则进入 waiting 停止尝试（等待用户重新切换开关）——
    // 不得对 UObject 注册表做每帧 FindFirstOf 轮询。
    constexpr auto kRetryInterval = std::chrono::seconds{2};
    constexpr std::uint32_t kMaximumUnavailableAttempts = 15;

    if (!worldContextReady) {
        return;
    }
    if (fishingBoostDirty_.exchange(false, std::memory_order_acq_rel)) {
        fishingBoostLedger_.set_desired(requestedFishingBoost_.load(std::memory_order_acquire));
        fishingSystemUnavailableCount_ = 0;
        nextFishingSystemAttempt_ = std::chrono::steady_clock::now();
    }
    if (fishingBoostLedger_.safety_disabled()) {
        fishingBoostPhase_.store(fishing_boost::Phase::safetyDisabled, std::memory_order_release);
        return;
    }
    const bool wantsOn = fishingBoostLedger_.desired() && !fishingBoostLedger_.has_records();
    const bool wantsOff = !fishingBoostLedger_.desired() && fishingBoostLedger_.has_records();
    if (wantsOn && std::chrono::steady_clock::now() < nextFishingSystemAttempt_) {
        return;  // 节流检查最先：窗口内常量时间返回，不做任何对象查找。
    }
    if (wantsOn || wantsOff) {
        // 世界上下文只在真正要执行事务的帧解析（与远程终端同款的 inventory 派生）。
        auto* const worldContext = UObjectGlobals::FindFirstOf(pal_game::kInventoryClassName);
        if (wantsOn) {
            if (fishing_boost::apply(fishingBoostLedger_, worldContext) ==
                fishing_boost::GatewayStatus::targetUnavailable) {
                ++fishingSystemUnavailableCount_;
                if (fishingSystemUnavailableCount_ >= kMaximumUnavailableAttempts) {
                    nextFishingSystemAttempt_ = std::chrono::steady_clock::time_point::max();
                    fishingBoostPhase_.store(fishing_boost::Phase::waiting,
                                             std::memory_order_release);
                } else {
                    nextFishingSystemAttempt_ = std::chrono::steady_clock::now() + kRetryInterval;
                }
                return;
            }
            fishingSystemUnavailableCount_ = 0;  // 成功或结构失败：重试链终止，计数复位。
        } else {
            static_cast<void>(fishing_boost::restore(fishingBoostLedger_, worldContext));
        }
    }
    fishingBoostPhase_.store(fishingBoostLedger_.phase(), std::memory_order_release);
}

auto PalworldEditorMod::restore_revive_timer_overrides(const std::string_view reason) -> bool {
    reviveTimerLedger_.set_desired(false);
    requestedReviveTimerRemove_.store(false, std::memory_order_release);
    const auto original = reviveTimerLedger_.original();
    if (!original.has_value()) {
        reviveTimerPhase_.store(reviveTimerLedger_.phase(worldSession_.generation()),
                                std::memory_order_release);
        return true;
    }

    auto result = revive_timer::restore_revive_timer_override(*original);
    const bool succeeded =
        result.status == revive_timer::ReviveTimerGatewayStatus::succeeded ||
        result.status == revive_timer::ReviveTimerGatewayStatus::verifiedRollback;
    static_cast<void>(reviveTimerLedger_.complete_restore(succeeded));
    std::string message{reason};
    message.append("：");
    message.append(result.message);
    publish_revive_timer_status(std::move(message));
    reviveTimerPhase_.store(reviveTimerLedger_.phase(worldSession_.generation()),
                            std::memory_order_release);
    return succeeded;
}

auto PalworldEditorMod::publish_revive_timer_status(std::string message) -> void {
    const std::lock_guard lock(reviveTimerStatusMutex_);
    reviveTimerStatus_ = std::move(message);
}

auto PalworldEditorMod::process_grapple_work(const float deltaSeconds) -> void {
    if (!worldLifecycleCallbacksReady_.load(std::memory_order_acquire)) {
        grappleRuntimePhase_.store(grappling_hook::CooldownRuntimePhase::waitingForWorld,
                                   std::memory_order_release);
        return;
    }
    if (grappleSafetyDisabled_.load(std::memory_order_acquire)) {
        grappleRuntimePhase_.store(grappling_hook::CooldownRuntimePhase::safetyDisabled,
                                   std::memory_order_release);
        return;
    }
    const auto worldGeneration = worldSession_.generation();
    const auto worldAccessible = worldSession_.can_access_unreal();
    const auto work = grappleLedger_.next_work(worldGeneration, worldAccessible);
    if (work == grappling_hook::CooldownWork::none) {
        grappleRuntimePhase_.store(grappleLedger_.phase(worldGeneration),
                                   std::memory_order_release);
        return;
    }
    if (work == grappling_hook::CooldownWork::restore) {
        const auto result = grappleGateway_.restore(grappleLedger_.records());
        grappleLedger_.complete_restore(result.succeeded());
        set_grapple_runtime_status(result.message);
        if (!result.succeeded()) {
            grappleSafetyDisabled_.store(true);
            requestedGrappleNoCooldown_.store(false);
            grappleLedger_.set_desired(false);
        }
        grappleRuntimePhase_.store(grappleSafetyDisabled_.load()
                                       ? grappling_hook::CooldownRuntimePhase::safetyDisabled
                                       : grappleLedger_.phase(worldGeneration),
                                   std::memory_order_release);
        return;
    }

    if (!grappleReadinessScheduler_.advance(deltaSeconds, worldGeneration, true)) {
        grappleRuntimePhase_.store(grappleLedger_.phase(worldGeneration),
                                   std::memory_order_release);
        return;
    }
    const auto commonInventoryReady = pal_game::is_valid(pal_game::get_main_container());
    worldInitializationReady_ = worldInitializationReady_ || commonInventoryReady;
    const auto ready = grappling_hook::grapple_apply_ready(
        worldLifecycleCallbacksReady_.load(std::memory_order_acquire), worldAccessible,
        commonInventoryReady);
    grappleReadinessScheduler_.complete(worldGeneration, ready);
    if (!ready) {
        set_grapple_runtime_status(
            "正在等待进入可访问世界并加载本地玩家 Common 主背包；不会逐帧扫描。");
        grappleRuntimePhase_.store(grappleLedger_.phase(worldGeneration),
                                   std::memory_order_release);
        return;
    }
    if (!grappleLedger_.begin_apply(worldGeneration)) {
        grappleReadinessScheduler_.request(worldGeneration);
        grappleRuntimePhase_.store(grappleLedger_.phase(worldGeneration),
                                   std::memory_order_release);
        return;
    }
    grappleRuntimePhase_.store(grappling_hook::CooldownRuntimePhase::applying,
                               std::memory_order_release);
    auto result = grappleGateway_.apply();
    const auto rollbackRecords = result.records;
    const auto accepted = grappleLedger_.complete_apply(
        worldGeneration, grappling_hook::to_apply_outcome(result.status),
        std::move(result.records));
    if (!accepted) {
        if (!rollbackRecords.empty()) {
            const auto restoreResult = grappleGateway_.restore(rollbackRecords);
            set_grapple_runtime_status(restoreResult.succeeded()
                                           ? "爪钩应用请求已过期；刚建立的覆盖已按原值恢复。"
                                           : "爪钩应用请求已过期，且即时恢复未能完整验证。");
            if (!restoreResult.succeeded()) {
                grappleSafetyDisabled_.store(true, std::memory_order_release);
            }
        } else {
            set_grapple_runtime_status(
                "爪钩覆盖返回了无效结果；本世界已安全停用，未保留覆盖记录。");
        }
    } else {
        set_grapple_runtime_status(std::move(result.message));
    }
    grappleRuntimePhase_.store(grappleSafetyDisabled_.load(std::memory_order_acquire)
                                   ? grappling_hook::CooldownRuntimePhase::safetyDisabled
                                   : grappleLedger_.phase(worldGeneration),
                               std::memory_order_release);
}

auto PalworldEditorMod::restore_grapple_overrides(const std::string_view reason) -> bool {
    if (grappleLedger_.records().empty()) {
        return true;
    }
    const auto result = grappleGateway_.restore(grappleLedger_.records());
    grappleLedger_.complete_restore(result.succeeded());
    if (result.succeeded()) {
        set_grapple_runtime_status(result.message);
        return true;
    }

    std::string message{"爪钩冷却在"};
    message.append(reason);
    message.append("前未能完整恢复；已停用后续覆盖。");
    set_grapple_runtime_status(std::move(message));
    return false;
}

auto PalworldEditorMod::begin_world_transition() -> void {
    const auto nextWorldGeneration = worldSession_.generation() + 1;
    worldInitializationReady_ = false;
    inventoryWritesDisabled_.store(false, std::memory_order_release);
    if (!restore_grapple_overrides("世界切换")) {
        grappleSafetyDisabled_.store(true);
        requestedGrappleNoCooldown_.store(false);
        grappleLedger_.set_desired(false);
        // 当前世界即将销毁，无法恢复的对象不会跨世界存活；清除旧路径但保留安全停用状态。
        grappleLedger_.complete_restore(true);
    }
    // 静态物品数据可能跨地图存活；切图前按精确账本恢复，失败时保留责任并停用再次应用。
    stack_limit_ledger_.set_desired(false);
    static_cast<void>(restore_stack_limit_overrides("世界切换"));
    requested_stack_unlimited_.store(false, std::memory_order_release);
    stack_setting_dirty_.store(false, std::memory_order_release);
    if (stack_limit_ledger_.records().empty()) {
        static_cast<void>(stack_limit_ledger_.begin_world(nextWorldGeneration));
    }
    // 游戏设置实例随世界重建；切图前恢复原值，不可解析时由新世界实例的原生值接管。
    reviveTimerLedger_.set_desired(false);
    static_cast<void>(restore_revive_timer_overrides("世界切换"));
    requestedReviveTimerRemove_.store(false, std::memory_order_release);
    reviveTimerSettingDirty_.store(false, std::memory_order_release);
    reviveTimerRetryRequested_.store(false, std::memory_order_release);
    static_cast<void>(reviveTimerLedger_.begin_world(nextWorldGeneration));
    reviveTimerPhase_.store(reviveTimerLedger_.phase(nextWorldGeneration),
                            std::memory_order_release);
    // 钓鱼圣手的目标 UPalWorldSubsystem 随世界销毁：切图前尽力恢复原值，账本随
    // 新世界重置（新世界实例天然使用原生值）。worldContext 仍属旧世界，恢复经
    // GetWorld 比对作用于当前实例；解析失败时新世界原生值接管，无需恢复。
    fishingBoostLedger_.set_desired(false);
    static_cast<void>(fishing_boost::restore(
        fishingBoostLedger_, UObjectGlobals::FindFirstOf(pal_game::kInventoryClassName)));
    requestedFishingBoost_.store(false, std::memory_order_release);
    fishingBoostDirty_.store(false, std::memory_order_release);
    fishingBoostLedger_.begin_world();
    fishingBoostPhase_.store(fishingBoostLedger_.phase(), std::memory_order_release);
    fishingSystemUnavailableCount_ = 0;
    nextFishingSystemAttempt_ = {};
    static_cast<void>(grappleLedger_.begin_world(nextWorldGeneration));
    grappleReadinessScheduler_.begin_world(nextWorldGeneration);
    grappleRuntimePhase_.store(grappleLedger_.phase(nextWorldGeneration),
                               std::memory_order_release);
    baseResourceBridge_.on_world_begin(worldSession_.generation() + 1);
    remotePalboxRuntime_.begin_world_transition();
    waypointTeleportRuntime_.begin_world_transition();
    captureRuntime_.on_world_end();
    captureRuntimePhase_.store(captureRuntime_.phase(), std::memory_order_release);
    worldSession_.begin_transition();
    statWritesDisabledForWorld_ = false;
    workSuitabilityWritesDisabledForWorld_ = false;
    identityWritesDisabledForWorld_ = false;
    passiveClassificationJob_.cancel();
    passiveClassificationCompleted_.store(0, std::memory_order_relaxed);
    passiveClassificationTotal_.store(0, std::memory_order_relaxed);
    hadUsablePassiveClassificationBeforeRefresh_ = false;
    skillQueue_.clear();
    statRequestSlot_.clear();
    identityRequestSlot_.clear();
    {
        const std::lock_guard lock(selectionRequestMutex_);
        selectCurrentPalRequest_.reset();
    }

    give_requested_.store(false);
    modify_requested_.store(false);
    want_read_.store(false);
    want_discover_.store(false);
    want_scan_items_.store(false);
    itemCatalogScanScheduler_.cancel();
    wantRefreshSkillCatalog_.store(false);
    wantProbeObject_.store(false);
    grappleRetryRequested_.store(false, std::memory_order_release);

    {
        const std::lock_guard lock(inv_mutex_);
        inv_cache_.clear();
        item_db_cache_ = {};
        selected_ = -1;
    }

    lastResolutionStatus_.reset();
    targetResolutionState_.reset();
    skillRuntimeSnapshot_.targetGeneration = selectedTarget_.generation();
    skillRuntimeSnapshot_.worldGeneration = worldSession_.generation();
    skillRuntimeSnapshot_.palName =
        selectedTarget_.is_selected() ? selectedTarget_.current().name : std::string{};
    skillRuntimeSnapshot_.state = {};
    skillRuntimeSnapshot_.palStat = {};
    skillRuntimeSnapshot_.palIdentity = {};
    skillRuntimeSnapshot_.catalog = {};
    skillRuntimeSnapshot_.lastResult =
        "世界切换已取消所有待处理操作；进入存档后请重新选择当前帕鲁。";
    skillRuntimeSnapshot_.resolutionStatus =
        skill_editor::SelectedTargetResolutionStatus::holderCandidatesUnavailable;
    skillRuntimeSnapshot_.targetSelected = selectedTarget_.is_selected();
    skillRuntimeSnapshot_.targetMatchesCurrent = false;
    skillRuntimeSnapshot_.pending = false;
    skillRuntimeSnapshot_.worldAccessible = false;
    skillRuntimeSnapshot_.worldLifecycleCallbacksReady = worldLifecycleCallbacksReady_.load();
    skillRuntimeSnapshot_.targetConfirmedForWorld = false;
    skillRuntimeSnapshot_.statWritesDisabled = false;
    skillRuntimeSnapshot_.workSuitabilityWritesDisabled = false;
    skillRuntimeSnapshot_.identityWritesDisabled = false;
    skillSnapshotDirty_ = true;
    publish_skill_snapshot_if_dirty();
}

auto PalworldEditorMod::finish_world_transition() -> void {
    skillQueue_.clear();
    statRequestSlot_.clear();
    identityRequestSlot_.clear();
    {
        const std::lock_guard lock(selectionRequestMutex_);
        selectCurrentPalRequest_.reset();
    }
    give_requested_.store(false);
    modify_requested_.store(false);
    want_discover_.store(false);
    worldInitializationReady_ = false;

    if (!worldLifecycleCallbacksReady_.load()) {
        worldSession_.begin_transition();
    }
    worldSession_.finish_transition();
    if (!stack_limit_ledger_.records().empty()) {
        // 对切图前已不可解析的对象只在新世界就绪事件再尝试一次，禁止退化为 EngineTick 轮询。
        stack_limit_ledger_.allow_restore_retry();
        static_cast<void>(restore_stack_limit_overrides("新世界就绪"));
    }
    if (stack_limit_ledger_.records().empty()) {
        static_cast<void>(stack_limit_ledger_.begin_world(worldSession_.generation()));
    }
    stack_limit_phase_.store(stack_limit_ledger_.phase(worldSession_.generation()),
                             std::memory_order_release);
    if (reviveTimerLedger_.original().has_value()) {
        // 对切图前已不可解析的设置实例只在新世界就绪事件再尝试一次，禁止退化为 EngineTick 轮询。
        reviveTimerLedger_.allow_restore_retry();
        static_cast<void>(restore_revive_timer_overrides("新世界就绪"));
    }
    static_cast<void>(reviveTimerLedger_.begin_world(worldSession_.generation()));
    reviveTimerPhase_.store(reviveTimerLedger_.phase(worldSession_.generation()),
                            std::memory_order_release);
    remotePalboxRuntime_.finish_world_transition();
    waypointTeleportRuntime_.finish_world_transition();
    baseResourceBridge_.on_world_ready(worldSession_.generation());
    captureRuntime_.on_world_begin();
    captureRuntimePhase_.store(captureRuntime_.phase(), std::memory_order_release);
    itemCatalogScanScheduler_.begin_world(worldSession_.generation());
    want_read_.store(true);
    want_scan_items_.store(true);
    wantRefreshSkillCatalog_.store(true);

    targetResolutionState_.reset();
    skillRuntimeSnapshot_.worldGeneration = worldSession_.generation();
    skillRuntimeSnapshot_.worldAccessible = true;
    skillRuntimeSnapshot_.targetConfirmedForWorld = false;
    skillRuntimeSnapshot_.targetMatchesCurrent = false;
    skillRuntimeSnapshot_.pending = false;
    skillRuntimeSnapshot_.resolutionStatus =
        skill_editor::SelectedTargetResolutionStatus::holderCandidatesUnavailable;
    skillRuntimeSnapshot_.lastResult =
        "已进入新的世界；原帕鲁选择仅用于显示，请重新点击“选择当前帕鲁”。";
    skillSnapshotDirty_ = true;
    publish_skill_snapshot_if_dirty();
}

auto PalworldEditorMod::publish_skill_snapshot_if_dirty() -> void {
    if (!std::exchange(skillSnapshotDirty_, false)) {
        return;
    }
    auto publishedSnapshot = std::make_shared<SkillEditorSnapshot>(skillRuntimeSnapshot_);
    const std::lock_guard lock(skillSnapshotMutex_);
    skillSnapshot_ = std::move(publishedSnapshot);
}

/** @brief 把 UE4SS 所需入口符号导出到 Windows DLL。 */
#define PALWORLD_EDITOR_API __declspec(dllexport)
extern "C" {
/**
 * @brief 创建并向 UE4SS 交付一个 PalworldEditor mod 实例。
 * @return 新分配的 mod 基类指针；所有权转移给 UE4SS，最终必须传给 uninstall_mod()。
 */
PALWORLD_EDITOR_API CppUserModBase* start_mod() {
    return new PalworldEditorMod();
}

/**
 * @brief 销毁由 start_mod() 创建的 PalworldEditor mod 实例。
 * @param[in] mod 要销毁的拥有型指针；必须来自本 DLL 的 start_mod()，可为 `nullptr`。
 */
PALWORLD_EDITOR_API void uninstall_mod(CppUserModBase* mod) {
    auto* const self = static_cast<PalworldEditorMod*>(mod);
    if (self == nullptr) {
        return;
    }
    self->request_unload_cleanup();
    switch (self->wait_for_unload_cleanup(kUnloadCleanupTimeout)) {
        case mod_lifecycle::UnloadCleanupWaitResult::cleanupSucceeded:
            delete mod;
            return;
        case mod_lifecycle::UnloadCleanupWaitResult::cleanupFailed: {
            // 已判定失败：区分不可恢复损失与尝试耗尽两种成因（处置相同，仅诊断粒度）。
            const bool permanent = self->unload_failure_is_permanent();
            if (permanent) {
                Output::send<LogLevel::Error>(
                    STR("PalworldEditor: 卸载清理已判定失败（存在不可恢复的恢复损失，如捕获"
                        "事务已丢失）；放弃销毁实例以避免回调悬垂。\n"));
            } else {
                Output::send<LogLevel::Error>(
                    STR("PalworldEditor: 卸载清理已判定失败（尝试次数耗尽，仍有瞬态账本未"
                        "恢复）；放弃销毁实例以避免回调悬垂。\n"));
            }
            return;
        }
        case mod_lifecycle::UnloadCleanupWaitResult::timedOut:
            // 游戏线程未在期限内得出结论（如已停止 Tick）：保留实例与已固定的 DLL，放弃
            // 销毁，避免在游戏线程回调仍可能执行时释放对象；进程退出时统一回收。
            Output::send<LogLevel::Error>(STR(
                "PalworldEditor: 游戏线程未在期限内完成卸载清理；放弃销毁实例以避免回调悬垂。\n"));
            return;
    }
}
}  // extern "C"
