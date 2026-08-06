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
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <DynamicOutput/DynamicOutput.hpp>
#include <GUI/GUITab.hpp>
#include <UE4SSProgram.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <common/text_encoding.hpp>
#include <mod/mod_core.hpp>
#include <skills/pal_resolution_scheduler.hpp>

using namespace RC;
using namespace RC::Unreal;

PalworldEditorMod::PalworldEditorMod() : CppUserModBase() {
    ModName = STR("PalworldEditor");
    ModVersion = STR("1.6.10");
    ModDescription = STR("Item, Pal skill, and same-guild base resource editor for Palworld 1.0");
    ModAuthors = STR("with-fair-wind");

    Output::send<LogLevel::Verbose>(STR("PalworldEditor loaded (v1.6.10)\n"));

    register_tab(STR("PalworldEditor"), [](CppUserModBase* mod) {
        UE4SS_ENABLE_IMGUI()
        auto* self = static_cast<PalworldEditorMod*>(mod);
        render_main_window(self);
    });
}

PalworldEditorMod::~PalworldEditorMod() {
    baseResourceBridge_.shutdown_hooks();
    unregister_callback(engineTickCallbackId_);
    unregister_callback(loadMapPostCallbackId_);
    unregister_callback(loadMapPreCallbackId_);
}

auto PalworldEditorMod::on_unreal_init() -> void {
    const Hook::FCallbackOptions loadMapPreOptions{
        .bOnce = false,
        .bReadonly = true,
        .OwnerModName = STR("PalworldEditor"),
        .HookName = STR("WorldTransitionBegin"),
    };
    loadMapPreCallbackId_ = Hook::RegisterLoadMapPreCallback(
        [this](Hook::TCallbackIterationData<bool>&, UEngine*, FWorldContext&, FURL,
               UPendingNetGame*, FString&) { begin_world_transition(); },
        loadMapPreOptions);

    const Hook::FCallbackOptions loadMapPostOptions{
        .bOnce = false,
        .bReadonly = true,
        .OwnerModName = STR("PalworldEditor"),
        .HookName = STR("WorldTransitionFinish"),
    };
    loadMapPostCallbackId_ = Hook::RegisterLoadMapPostCallback(
        [this](Hook::TCallbackIterationData<bool>&, UEngine*, FWorldContext&, FURL,
               UPendingNetGame*, FString&) { finish_world_transition(); },
        loadMapPostOptions);

    worldLifecycleCallbacksReady_.store(loadMapPreCallbackId_ != Hook::ERROR_ID &&
                                        loadMapPostCallbackId_ != Hook::ERROR_ID);

    const Hook::FCallbackOptions engineTickOptions{
        .bOnce = false,
        .bReadonly = true,
        .OwnerModName = STR("PalworldEditor"),
        .HookName = STR("GameThreadTick"),
    };
    engineTickCallbackId_ = Hook::RegisterEngineTickPreCallback(
        [this](Hook::TCallbackIterationData<void>&, UEngine*, const float deltaSeconds, bool) {
            game_thread_tick(deltaSeconds);
        },
        engineTickOptions);

    wantProbeObject_.store(true);
    want_scan_items_.store(true);
    baseResourceBridge_.on_world_ready(worldSession_.generation());
    static_cast<void>(grappleLedger_.begin_world(worldSession_.generation()));
    grappleReadinessScheduler_.begin_world(worldSession_.generation());
    grappleRuntimePhase_.store(grappleLedger_.phase(worldSession_.generation()),
                               std::memory_order_release);

    if (engineTickCallbackId_ == Hook::ERROR_ID || !worldLifecycleCallbacksReady_.load()) {
        baseResourceBridge_.on_world_begin(worldSession_.generation() + 1);
        skillQueue_.clear();
        {
            const std::lock_guard lock(selectionRequestMutex_);
            selectCurrentPalRequest_.reset();
        }
        skillRuntimeSnapshot_.lastResult =
            "UE4SS 游戏线程/世界切换回调注册失败；为避免跨世界写入，技能编辑已停用。";
        skillSnapshotDirty_ = true;
        publish_skill_snapshot_if_dirty();
    }

    // 远程终端配置：mods/<ModName>/remote_palbox.ini（缺失时回退默认值）。
    const auto modsDirectory = UE4SSProgram::get_program().get_mods_directory();
    const auto iniPath = (std::filesystem::path(modsDirectory) / std::filesystem::path(ModName) /
                          L"remote_palbox.ini")
                             .wstring();
    remotePalboxRuntime_.load_config(text_encoding::to_utf8(iniPath));
}

auto PalworldEditorMod::on_update() -> void {}

auto PalworldEditorMod::game_thread_tick(const float deltaSeconds) -> void {
    if (!worldSession_.can_access_unreal()) {
        return;
    }

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
    process_grapple_work(deltaSeconds);
    baseResourceBridge_.ensure_hooks_registered();
    baseResourceBridge_.tick(deltaSeconds);
    remotePalboxRuntime_.tick(deltaSeconds, worldSession_);

    if (wantProbeObject_.exchange(false)) {
        if (const auto object = UObjectGlobals::StaticFindObject<UObject*>(
                nullptr, nullptr, STR("/Script/CoreUObject.Object"))) {
            Output::send<LogLevel::Verbose>(STR("Object Name: {}\n"), object->GetFullName());
        }
    }

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
    if (doGive) {
        pal_game::give_items(item, static_cast<int32>(count));
        want_read_.store(true);
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
    if (doMod) {
        pal_game::set_slot_count(modSlot, modCount);
        want_read_.store(true);
    }

    // 主菜单（无背包容器）时跳过物品扫描和背包读取，避免 fallback 日志噪音和无谓 ForEachUObject。
    const auto worldContextReady = pal_game::is_valid(pal_game::get_main_container());

    // Read inventory
    if (worldContextReady && want_read_.exchange(false)) {
        auto fresh = pal_game::read_inventory();
        const std::lock_guard lock(inv_mutex_);
        if (selected_ >= static_cast<int>(fresh.size())) {
            selected_ = -1;
        }
        inv_cache_ = std::move(fresh);
    }

    // Scan items. StaticItemDataMap may become ready after LoadMap, so fallback scans retry
    // with a bounded pure-value scheduler instead of probing UObject state every frame.
    const auto worldGeneration = worldSession_.generation();
    if (worldContextReady && want_scan_items_.exchange(false)) {
        itemCatalogScanScheduler_.request(worldGeneration);
    }
    if (worldContextReady &&
        itemCatalogScanScheduler_.advance(deltaSeconds, worldGeneration, true)) {
        auto result = pal_game::scan_all_items();
        static_cast<void>(
            itemCatalogScanScheduler_.complete(worldGeneration, result.usedStaticItemDataMap));
        const std::lock_guard lock(inv_mutex_);
        if (result.usedStaticItemDataMap || item_db_cache_.items.empty()) {
            item_db_cache_ = std::move(result.catalog);
        }
    }

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
            skillRuntimeSnapshot_.state = skillGateway_.read_state(
                reinterpret_cast<skill_editor::SkillTarget>(resolvedPal->parameter));
            skillRuntimeSnapshot_.palStat = statGateway_.read_stats(
                reinterpret_cast<pal_stats::PalStatTarget>(resolvedPal->parameter));
            skillRuntimeSnapshot_.palIdentity = identityGateway_.read_identity(
                reinterpret_cast<pal_identity::PalIdentityTarget>(resolvedPal->parameter),
                resolvedPal->spawnStateKnown, resolvedPal->selectedIsSpawned);
            skillRuntimeSnapshot_.lastResult.clear();
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

    const bool manualRefreshRequested = wantRefreshSkillCatalog_.exchange(false);
    const bool catalogReady =
        skill_editor::catalog_is_ready_for_editing(skillRuntimeSnapshot_.catalog);
    const bool refreshRequested = skillCatalogRefreshScheduler_.should_refresh(
        manualRefreshRequested, catalogReady,
        skill_editor::SkillCatalogRefreshScheduler::clock::now(),
        [worldContextReady] { return worldContextReady; });
    if (refreshRequested) {
        refresh_skill_catalog_on_game_thread();
    }
    advance_passive_classification_on_game_thread();

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

    // Discover
    if (want_discover_.exchange(false)) {
        pal_game::discover_objects();
    }
}

auto PalworldEditorMod::unregister_callback(Hook::GlobalCallbackId& callbackId) -> void {
    if (callbackId == Hook::ERROR_ID) {
        return;
    }
    if (!Hook::UnregisterCallback(callbackId)) {
        Output::send<LogLevel::Warning>(
            STR("PalworldEditor: failed to unregister callback id={}\n"), callbackId);
    }
    callbackId = Hook::ERROR_ID;
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
    if (!restore_grapple_overrides("世界切换")) {
        grappleSafetyDisabled_.store(true);
        requestedGrappleNoCooldown_.store(false);
        grappleLedger_.set_desired(false);
        // 当前世界即将销毁，无法恢复的对象不会跨世界存活；清除旧路径但保留安全停用状态。
        grappleLedger_.complete_restore(true);
    }
    static_cast<void>(grappleLedger_.begin_world(nextWorldGeneration));
    grappleReadinessScheduler_.begin_world(nextWorldGeneration);
    grappleRuntimePhase_.store(grappleLedger_.phase(nextWorldGeneration),
                               std::memory_order_release);
    baseResourceBridge_.on_world_begin(worldSession_.generation() + 1);
    remotePalboxRuntime_.begin_world_transition();
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

    if (!worldLifecycleCallbacksReady_.load()) {
        worldSession_.begin_transition();
    }
    worldSession_.finish_transition();
    remotePalboxRuntime_.finish_world_transition();
    baseResourceBridge_.on_world_ready(worldSession_.generation());
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
    const std::lock_guard lock(skillSnapshotMutex_);
    skillSnapshot_ = skillRuntimeSnapshot_;
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
    delete mod;
}
}  // extern "C"
