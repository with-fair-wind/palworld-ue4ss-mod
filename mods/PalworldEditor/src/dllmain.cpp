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
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <DynamicOutput/DynamicOutput.hpp>
#include <GUI/GUITab.hpp>
#include <Mod/CppUserModBase.hpp>
#include <UE4SSProgram.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <base_resource_sharing/pal_base_resources.hpp>
#include <game/pal_game.hpp>
#include <grappling_hook/cooldown_gateway.hpp>
#include <imgui.h>
#include <items/item_catalog.hpp>
#include <pal_stats/pal_stat_editor.hpp>
#include <pal_stats/pal_stats.hpp>
#include <skills/pal_resolution_scheduler.hpp>
#include <skills/pal_skills.hpp>
#include <skills/passive_skill_presets.hpp>
#include <skills/selected_target_state.hpp>
#include <skills/world_session_state.hpp>
#include <support/text_encoding.hpp>

using namespace RC;
using namespace RC::Unreal;
using pal_game::InvEntry;

/**
 * @brief PalworldEditor 的 UE4SS mod 实例与全部运行时状态容器。
 * @details 类本身拥有 GUI 状态、请求队列和值类型缓存，但不拥有任何 Unreal UObject。
 *          GUI 线程不得直接调用反射接口；游戏线程通过 EngineTick 回调消费请求并发布快照。
 */
class PalworldEditorMod final : public CppUserModBase {
public:
    /**
     * @brief 初始化 mod 元数据并注册 `PalworldEditor` ImGui 页签。
     * @details 构造阶段不访问 Unreal UObject；页签回调只调用本类的静态渲染辅助函数。
     */
    PalworldEditorMod() : CppUserModBase() {
        ModName = STR("PalworldEditor");
        ModVersion = STR("1.6.10");
        ModDescription =
            STR("Item, Pal skill, and same-guild base resource editor for Palworld 1.0");
        ModAuthors = STR("with-fair-wind");

        Output::send<LogLevel::Verbose>(STR("PalworldEditor loaded (v1.6.10)\n"));

        register_tab(STR("PalworldEditor"), [](CppUserModBase* mod) {
            UE4SS_ENABLE_IMGUI()
            auto* self = static_cast<PalworldEditorMod*>(mod);
            ImGui::TextUnformatted("应可见一个浮动的「PalworldEditor」窗口 ->");
            if (ImGui::Begin("PalworldEditor v1.6.10", nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
                render_give_items(self);
                ImGui::Separator();
                render_item_browser(self);
                ImGui::Separator();
                render_inventory(self);
                ImGui::Separator();
                render_base_resource_sharing(self);
                ImGui::Separator();
                render_grapple_no_cooldown(self);
                ImGui::Separator();
                render_pal_editor(self);
                ImGui::Separator();
                if (ImGui::Button("发现对象")) {
                    self->want_discover_.store(true);
                }
            }
            ImGui::End();
        });
    }

    /** @brief 注销本实例注册的 UE4SS 全局回调。 */
    ~PalworldEditorMod() override {
        baseResourceBridge_.shutdown_hooks();
        unregister_callback(engineTickCallbackId_);
        unregister_callback(loadMapPostCallbackId_);
        unregister_callback(loadMapPreCallbackId_);
    }

    /**
     * @brief 在 UE4SS 完成 Unreal 初始化后注册游戏线程与世界切换回调。
     * @details 此处只注册回调并发布值请求；对象探测延迟到首次 EngineTick 执行。
     */
    auto on_unreal_init() -> void override {
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
    }

    /**
     * @brief UE4SS UpdateThread 回调；刻意不访问 Unreal 或运行时请求队列。
     */
    auto on_update() -> void override {}

    /**
     * @brief 在 EngineTick 游戏线程消费全部 GUI 请求、执行反射操作并发布最新快照。
     * @details 单次更新依次处理给予物品、背包数量修改、背包/物品扫描、当前待出战帕鲁解析、
     *          技能编辑 FIFO 队列、技能目录/状态刷新和诊断扫描。共享结果在相应互斥量保护下写回。
     * @warning 这是本类调用 Palworld 反射适配接口的唯一周期入口。
     */
    auto game_thread_tick(const float deltaSeconds) -> void {
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

        // Read inventory
        if (want_read_.exchange(false)) {
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
        if (want_scan_items_.exchange(false)) {
            itemCatalogScanScheduler_.request(worldGeneration);
        }
        if (itemCatalogScanScheduler_.advance(deltaSeconds, worldGeneration, true)) {
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
        if (selectionRequested) {
            skillQueue_.clear();
            statRequestSlot_.clear();
        } else {
            editRequest = skillQueue_.try_pop();
            statRequest = statRequestSlot_.consume();
        }

        const auto trigger = skill_editor::decide_pal_resolution(
            selectionRequested, editRequest.has_value() || statRequest.has_value());
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
            skillSnapshotDirty_ =
                targetResolutionState_.update(nextResolution) || skillSnapshotDirty_;

            if (!lastResolutionStatus_.has_value() ||
                *lastResolutionStatus_ != resolvedPal->status) {
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
                skillRuntimeSnapshot_.lastResult.clear();
            } else {
                skillRuntimeSnapshot_.palStat = {};
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
            const auto target =
                resolvedPal.has_value() && resolution.resolved
                    ? reinterpret_cast<pal_stats::PalStatTarget>(resolvedPal->parameter)
                    : pal_stats::PalStatTarget{};
            const bool targetCurrent = skill_editor::bound_target_request_is_current(
                *statRequest, selectedTarget_, resolution.observation, target, worldSession_);
            if (targetCurrent && !statWritesDisabledForWorld_ &&
                pal_stats::has_any_change(statRequest->values) && statGateway_.is_valid(target)) {
                const auto result = statGateway_.apply_stat_edit(target, *statRequest);
                skillRuntimeSnapshot_.palStat = result.snapshot;
                skillRuntimeSnapshot_.lastResult = result.message;
                if (result.status == pal_stats::PalStatEditStatus::rollbackFailed) {
                    statWritesDisabledForWorld_ = true;
                }
            } else {
                statRequestSlot_.clear();
                skillRuntimeSnapshot_.lastResult =
                    statWritesDisabledForWorld_
                        ? "本世界曾发生属性恢复验证失败；后续属性写入已安全停用。"
                        : "当前高亮帕鲁与已选择目标不一致或暂时无法确认；"
                          "属性修改未执行。";
            }
            skillSnapshotDirty_ = true;
        }

        const bool manualRefreshRequested = wantRefreshSkillCatalog_.exchange(false);
        const bool catalogReady =
            skill_editor::catalog_is_ready_for_editing(skillRuntimeSnapshot_.catalog);
        const bool refreshRequested = skillCatalogRefreshScheduler_.should_refresh(
            manualRefreshRequested, catalogReady,
            skill_editor::SkillCatalogRefreshScheduler::clock::now(),
            [] { return pal_game::is_valid(pal_game::get_main_container()); });
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
        update_runtime_value(skillRuntimeSnapshot_.worldAccessible,
                             worldSession_.can_access_unreal());
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

private:
    /** @brief 每个 EngineTick 最多尝试分类的被动技能数量。 */
    static constexpr std::size_t kPassiveMetadataBatchSize = 8;
    /** @brief 每个 EngineTick 被动技能分类反射的软时间预算。 */
    static constexpr auto kPassiveMetadataBudget = std::chrono::microseconds{500};

    /** @brief Unregisters one owned UE4SS callback if registration succeeded. */
    static auto unregister_callback(Hook::GlobalCallbackId& callbackId) -> void {
        if (callbackId == Hook::ERROR_ID) {
            return;
        }
        if (!Hook::UnregisterCallback(callbackId)) {
            Output::send<LogLevel::Warning>(
                STR("PalworldEditor: failed to unregister callback id={}\n"), callbackId);
        }
        callbackId = Hook::ERROR_ID;
    }

    /**
     * @brief 在游戏线程刷新技能目录，并为成功的被动目录建立增量分类任务。
     * @details 目录失败时沿用既有分类任务和回退快照；成功时先合并生命周期缓存，
     *          仅把未缓存 ID 排入后续 EngineTick。
     */
    auto refresh_skill_catalog_on_game_thread() -> void {
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

    /**
     * @brief 在当前 EngineTick 推进最多一个受数量和时间约束的分类批次。
     */
    auto advance_passive_classification_on_game_thread() -> void {
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

    /**
     * @brief 合并分类结果、应用失败回退并只发布一次最终目录快照。
     */
    auto finish_passive_classification_on_game_thread() -> void {
        skill_editor::apply_passive_metadata(skillRuntimeSnapshot_.catalog.passive.skills,
                                             passiveSkillMetadataCache_);
        auto status = skill_editor::with_passive_classification_fallback(
            passiveClassificationJob_.status(), hadUsablePassiveClassificationBeforeRefresh_);
        skillRuntimeSnapshot_.catalog.passiveClassification = status;
        passiveClassificationCompleted_.store(status.completed, std::memory_order_relaxed);
        passiveClassificationTotal_.store(status.total, std::memory_order_relaxed);
        skillSnapshotDirty_ = true;

        const auto known = std::ranges::count_if(skillRuntimeSnapshot_.catalog.passive.skills,
                                                 [](const skill_editor::SkillOption& option) {
                                                     return option.passiveMetadata.has_value();
                                                 });
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

    /** @brief 把爪钩游戏线程结果发布为 GUI 可安全复制的纯字符串。 */
    auto set_grapple_runtime_status(std::string status) -> void {
        const std::lock_guard lock(grappleStatusMutex_);
        grappleRuntimeStatus_ = std::move(status);
    }

    /**
     * @brief 在 EngineTick 执行一次由纯值账本决定的爪钩应用或恢复工作。
     * @details 默认关闭、已应用和世界不可访问时立即返回，不扫描 UObject。
     */
    auto process_grapple_work(const float deltaSeconds) -> void {
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

    /**
     * @brief 在关闭开关或切图前恢复全部活动爪钩覆盖。
     * @param[in] reason 用于错误消息的生命周期阶段。
     * @return 没有覆盖或恢复验证成功时返回 true。
     */
    [[nodiscard]] auto restore_grapple_overrides(const std::string_view reason) -> bool {
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

    /** @brief Invalidates all work and write authorization before Unreal replaces the world. */
    auto begin_world_transition() -> void {
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
        worldSession_.begin_transition();
        statWritesDisabledForWorld_ = false;
        passiveClassificationJob_.cancel();
        passiveClassificationCompleted_.store(0, std::memory_order_relaxed);
        passiveClassificationTotal_.store(0, std::memory_order_relaxed);
        hadUsablePassiveClassificationBeforeRefresh_ = false;
        skillQueue_.clear();
        statRequestSlot_.clear();
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
        skillSnapshotDirty_ = true;
        publish_skill_snapshot_if_dirty();
    }

    /** @brief Re-enables reads after LoadMap without restoring Pal write authorization. */
    auto finish_world_transition() -> void {
        skillQueue_.clear();
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

    /**
     * @brief 游戏线程发布给 GUI 的完整技能编辑快照。
     * @details 此结构按值复制，避免 GUI 在持锁期间执行复杂渲染逻辑。
     */
    struct SkillEditorSnapshot {
        std::uint64_t targetGeneration{};           /**< GUI 提交请求时使用的纯值目标代数。 */
        std::uint64_t worldGeneration{};            /**< GUI 提交请求时使用的世界代次。 */
        std::string palName;                        /**< 当前显式确认目标的 GUI 展示名称。 */
        skill_editor::SkillState state;             /**< 最近一次从游戏重读的实际技能状态。 */
        skill_editor::SkillCatalogSnapshot catalog; /**< 最近一份可用的运行时技能目录。 */
        std::string lastResult;                     /**< 最近一次技能编辑结果的面向用户消息。 */
        skill_editor::SelectedTargetResolutionStatus resolutionStatus{
            skill_editor::SelectedTargetResolutionStatus::
                holderCandidatesUnavailable}; /**< 当前解析状态。 */
        bool targetSelected{};                /**< 是否存在用户显式确认的技能目标。 */
        bool targetMatchesCurrent{};          /**< 当前高亮目标是否与显式确认的 GUID 相同。 */
        bool pending{};                       /**< 技能请求队列中是否仍有待游戏线程处理的请求。 */
        bool worldAccessible{true};           /**< 当前是否不处于 LoadMap 过渡阶段。 */
        bool worldLifecycleCallbacksReady{};  /**< LoadMap 前后回调是否均已注册。 */
        bool targetConfirmedForWorld{};       /**< 当前世界是否已由用户重新确认目标。 */
        bool statWritesDisabled{};            /**< 本世界是否因恢复验证失败而停用属性写入。 */
        pal_stats::PalStatSnapshot palStat;   /**< 最近一次从游戏重读的实际属性值。 */
    };

    /** @brief 仅在可观察技能状态变化时把游戏线程快照发布给 GUI。 */
    auto publish_skill_snapshot_if_dirty() -> void {
        if (!std::exchange(skillSnapshotDirty_, false)) {
            return;
        }
        const std::lock_guard lock(skillSnapshotMutex_);
        skillSnapshot_ = skillRuntimeSnapshot_;
    }

    /**
     * @brief 把整数限制到闭区间 `[lo, hi]`。
     * @param[in] v 待限制的输入值。
     * @param[in] lo 允许的最小值。
     * @param[in] hi 允许的最大值；调用方保证 `lo <= hi`。
     * @return 限制后的整数。
     */
    static auto clamp(int v, int lo, int hi) -> int {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    /**
     * @brief 在技能目录中查找 Raw ID 对应的本地化标签。
     * @param[in] options 要搜索的技能目录值列表。
     * @param[in] id 技能 Raw ID。
     * @return 找到时返回 `本地化名称 [RawId]`，未找到时回退为原始 ID。
     */
    static auto find_skill_label(const std::vector<skill_editor::SkillOption>& options,
                                 const std::string_view id) -> std::string {
        const auto found = std::ranges::find(options, id, &skill_editor::SkillOption::id);
        return found == options.end() ? std::string(id) : skill_editor::skill_label(*found);
    }

    /**
     * @brief 在技能目录中查找 Raw ID 对应的目录项。
     * @param[in] options 要搜索的技能目录值列表。
     * @param[in] id 技能 Raw ID。
     * @return 找到时返回非空、非拥有的目录项指针；未找到时返回 `nullptr`。
     */
    [[nodiscard]] static auto find_skill_option(
        const std::vector<skill_editor::SkillOption>& options, const std::string_view id)
        -> const skill_editor::SkillOption* {
        const auto found = std::ranges::find(options, id, &skill_editor::SkillOption::id);
        return found == options.end() ? nullptr : &*found;
    }

    /**
     * @brief 清空与上一个技能目标相关的 GUI 临时编辑状态。
     * @param[in,out] self 非空、非拥有的当前 mod 实例指针。
     * @warning 只在 GUI 线程调用。
     */
    static void reset_skill_editor_ui(PalworldEditorMod* self) {
        self->passivePresetIndex_.reset();
        self->passiveEditIndex_ = -1;
        self->activeEditSlot_ = -1;
        self->passivePickerState_.reset();
        self->activeChoice_.reset();
        self->passiveSearch_[0] = '\0';
        self->activeSearch_[0] = '\0';
        self->statDraft_.reset();
    }

    /**
     * @brief 渲染支持搜索、排除和单选的技能下拉框。
     * @param[in] id ImGui 控件的唯一标识。
     * @param[in] options 可供选择的技能目录。
     * @param[in] excludedIds 不应出现在结果中的已装备 Raw ID 集合。
     * @param[in,out] search GUI 搜索缓冲区。
     * @param[in] searchSize 搜索缓冲区容量（含终止空字符）。
     * @param[in,out] selected 当前选择项；用户选择新项时被替换。
     * @retval true 本帧用户选择了不同的目录项。
     * @retval false 选择未发生变化。
     * @warning `search`、`selected` 和调用者状态只允许由 GUI 线程访问。
     */
    static auto render_skill_picker(const char* id,
                                    const std::vector<skill_editor::SkillOption>& options,
                                    const std::unordered_set<std::string>& excludedIds,
                                    char* search, const std::size_t searchSize,
                                    std::optional<skill_editor::SkillOption>& selected) -> bool {
        const std::string preview =
            selected.has_value() ? skill_editor::skill_label(*selected) : "请选择技能";
        bool changed = false;
        if (ImGui::BeginCombo(id, preview.c_str())) {
            ImGui::SetNextItemWidth(340.0F);
            ImGui::InputText("搜索##skill-search", search, searchSize);
            const auto visible = skill_editor::filter_skills(options, search, excludedIds);
            for (const auto& option : visible) {
                const auto label = skill_editor::skill_label(option);
                const bool isSelected = selected.has_value() && selected->id == option.id;
                if (ImGui::Selectable(label.c_str(), isSelected)) {
                    selected = option;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        return changed;
    }

    /**
     * @brief 渲染物品 Raw ID 与数量输入，并提交给予物品请求。
     * @param[in,out] self 非空、非拥有的当前 mod 实例指针。
     * @details 点击 Give 时只在 req_mutex_ 保护下复制请求参数并设置原子标志，不调用 Unreal。
     * @warning 只在 GUI 线程调用。
     */
    static void render_give_items(PalworldEditorMod* self) {
        ImGui::TextUnformatted("给予物品");
        ImGui::InputText("物品 ID", self->item_buf_, sizeof(self->item_buf_));
        ImGui::InputInt("数量", &self->count_input_);
        self->count_input_ = clamp(self->count_input_, 1, 9999);
        if (ImGui::Button("给予")) {
            const std::lock_guard lock(self->req_mutex_);
            self->give_item_ = self->item_buf_;
            self->give_count_ = self->count_input_;
            self->give_requested_ = true;
        }
    }

    /**
     * @brief 渲染可搜索的物品目录并把选中项的 Raw ID 填入给予输入框。
     * @param[in,out] self 非空、非拥有的当前 mod 实例指针。
     * @details 列表展示本地化标签，但点击时只复制 `ItemOption::id`；目录缓存读取受
     *          inv_mutex_ 保护，重新扫描通过原子请求交给游戏线程。
     * @warning 只在 GUI 线程调用。
     */
    static void render_item_browser(PalworldEditorMod* self) {
        if (ImGui::Button("扫描游戏物品")) {
            self->want_scan_items_.store(true);
        }
        ImGui::SameLine();
        ImGui::InputText("##search", self->search_buf_, sizeof(self->search_buf_));
        {
            const std::lock_guard lock(self->inv_mutex_);
            ImGui::TextDisabled("（%d 件物品）",
                                static_cast<int>(self->item_db_cache_.items.size()));
        }
        ImGui::BeginChild("browser", ImVec2(380, 160), true);
        {
            const std::lock_guard lock(self->inv_mutex_);
            if (self->item_db_cache_.items.empty()) {
                ImGui::TextDisabled("尚未发现物品，请重新扫描。");
            }
            const auto visible =
                item_catalog::filter_items(self->item_db_cache_, self->search_buf_);
            for (const auto* item : visible) {
                const auto label = item_catalog::item_label(*item);
                if (ImGui::Selectable(label.c_str())) {
                    const auto copyLen = std::min(item->id.size(), sizeof(self->item_buf_) - 1);
                    std::memcpy(self->item_buf_, item->id.data(), copyLen);
                    self->item_buf_[copyLen] = '\0';
                }
            }
        }
        ImGui::EndChild();
    }

    /**
     * @brief 渲染主背包快照、当前选择以及槽位数量修改请求。
     * @param[in,out] self 非空、非拥有的当前 mod 实例指针。
     * @details 展示使用本地化标签，真正提交的修改键是 InvEntry::slot_index；缓存读取受
     *          inv_mutex_ 保护，请求参数写入受 req_mutex_ 保护。
     * @warning 只在 GUI 线程调用，不直接写 Unreal 属性。
     */
    static void render_inventory(PalworldEditorMod* self) {
        if (ImGui::Button("刷新背包")) {
            self->want_read_.store(true);
        }
        ImGui::SameLine();
        ImGui::TextUnformatted("（点击物品选中，再设置数量）");
        {
            const std::lock_guard lock(self->inv_mutex_);
            ImGui::BeginChild("invlist", ImVec2(380, 220), true);
            for (int i = 0; i < static_cast<int>(self->inv_cache_.size()); ++i) {
                const auto& e = self->inv_cache_[i];
                const auto itemLabel = item_catalog::item_label(self->item_db_cache_, e.item_id);
                const auto label =
                    itemLabel + "  x" + std::to_string(e.count) + " ##inv" + std::to_string(i);
                if (ImGui::Selectable(label.c_str(), self->selected_ == i)) {
                    self->selected_ = i;
                    self->set_count_input_ = e.count;
                }
            }
            ImGui::EndChild();

            if (self->selected_ >= 0 &&
                self->selected_ < static_cast<int>(self->inv_cache_.size())) {
                const auto& e = self->inv_cache_[self->selected_];
                const auto itemLabel = item_catalog::item_label(self->item_db_cache_, e.item_id);
                ImGui::Text("已选中：%s（槽位 %d，×%d）", itemLabel.c_str(),
                            static_cast<int>(e.slot_index), e.count);
                ImGui::InputInt("新数量", &self->set_count_input_);
                self->set_count_input_ = clamp(self->set_count_input_, 0, 9999);
                if (ImGui::Button("设置数量")) {
                    const std::lock_guard lock2(self->req_mutex_);
                    self->modify_slot_ = e.slot_index;
                    self->modify_count_ = self->set_count_input_;
                    self->modify_requested_ = true;
                }
            }
        }
    }

    /**
     * @brief 返回被动技能分类在界面上的固定中文名称。
     * @param[in] category 具体类别；空值表示“全部”。
     * @return 类别名称字面量；调用方无需释放。
     */
    [[nodiscard]] static auto passive_category_label(
        const std::optional<skill_editor::PassiveSkillCategory> category) -> const char* {
        if (!category.has_value()) {
            return "全部";
        }
        switch (*category) {
            case skill_editor::PassiveSkillCategory::normal:
                return "普通";
            case skill_editor::PassiveSkillCategory::rare:
                return "稀有";
            case skill_editor::PassiveSkillCategory::premium:
                return "极品";
            case skill_editor::PassiveSkillCategory::legendary:
                return "传说";
            case skill_editor::PassiveSkillCategory::negative:
                return "负面";
        }
        return "全部";
    }

    /**
     * @brief 返回被动技能分类在界面上的着色。
     * @param[in] category 具体类别。
     * @return 普通/稀有/极品/传说/负面分别对应白/黄/蓝/紫/红的 ImGui 颜色。
     */
    [[nodiscard]] static auto passive_category_color(
        const skill_editor::PassiveSkillCategory category) -> ImVec4 {
        switch (category) {
            case skill_editor::PassiveSkillCategory::normal:
                return {0.92F, 0.92F, 0.92F, 1.0F};
            case skill_editor::PassiveSkillCategory::rare:
                return {1.0F, 0.82F, 0.20F, 1.0F};
            case skill_editor::PassiveSkillCategory::premium:
                return {0.30F, 0.65F, 1.0F, 1.0F};
            case skill_editor::PassiveSkillCategory::legendary:
                return {0.72F, 0.40F, 1.0F, 1.0F};
            case skill_editor::PassiveSkillCategory::negative:
                return {1.0F, 0.30F, 0.30F, 1.0F};
        }
        return {1.0F, 1.0F, 1.0F, 1.0F};
    }

    /**
     * @brief 渲染被动技能类别下拉框和分类进度/错误提示。
     * @param[in,out] self 非空、非拥有的当前 mod 实例指针。
     * @param[in] catalog 当前技能目录快照。
     * @details 顺序固定为“全部”与五个具体类别；分类未就绪时具体类别禁用。进度和错误直接
     *          读取原子字段与分类状态，不复制完整技能目录。结构性失败但已有旧分类时具体类别
     *          仍可用，并提示“正在使用上一次成功分类”。
     * @warning 只在 GUI 线程调用。
     */
    static void render_passive_category_picker(PalworldEditorMod* self,
                                               const skill_editor::SkillCatalogSnapshot& catalog) {
        const auto& classification = catalog.passiveClassification;
        const bool ready = classification.ready;
        const auto completed =
            self->passiveClassificationCompleted_.load(std::memory_order_relaxed);
        const auto total = self->passiveClassificationTotal_.load(std::memory_order_relaxed);

        ImGui::SetNextItemWidth(160.0F);
        if (ImGui::BeginCombo("类别##passive-category",
                              passive_category_label(self->passivePickerState_.category))) {
            if (ImGui::Selectable("全部", !self->passivePickerState_.category.has_value())) {
                (void)self->passivePickerState_.set_category(std::nullopt);
            }
            const skill_editor::PassiveSkillCategory concreteCategories[] = {
                skill_editor::PassiveSkillCategory::normal,
                skill_editor::PassiveSkillCategory::rare,
                skill_editor::PassiveSkillCategory::premium,
                skill_editor::PassiveSkillCategory::legendary,
                skill_editor::PassiveSkillCategory::negative,
            };
            ImGui::BeginDisabled(!ready);
            for (const auto category : concreteCategories) {
                const bool isCurrent = self->passivePickerState_.category == category;
                ImGui::PushStyleColor(ImGuiCol_Text, passive_category_color(category));
                if (ImGui::Selectable(passive_category_label(category), isCurrent)) {
                    (void)self->passivePickerState_.set_category(category);
                }
                ImGui::PopStyleColor();
            }
            ImGui::EndDisabled();
            ImGui::EndCombo();
        }

        if (!ready && classification.error.empty()) {
            ImGui::TextDisabled("正在读取被动技能分类：%d/%d", static_cast<int>(completed),
                                static_cast<int>(total));
        } else if (!classification.error.empty()) {
            if (ready) {
                ImGui::TextColored(passive_category_color(skill_editor::PassiveSkillCategory::rare),
                                   "正在使用上一次成功分类（本次读取失败：%s）",
                                   classification.error.c_str());
            } else {
                ImGui::TextColored(
                    passive_category_color(skill_editor::PassiveSkillCategory::negative),
                    "被动技能分类失败：%s（仅“全部”可用）", classification.error.c_str());
            }
        }
    }

    /**
     * @brief 渲染按类别、排除集合和搜索词过滤的被动技能下拉框。
     * @param[in,out] self 非空、非拥有的当前 mod 实例指针。
     * @param[in] options 当前被动技能目录。
     * @param[in] excludedIds 已装备且不应再次选择的被动技能 Raw ID。
     * @retval true 本帧用户选择了不同的目录项。
     * @retval false 选择未发生变化。
     * @details 搜索同时匹配中文名与 Raw ID；有分类元数据的条目按类别着色，未知元数据在“全部”
     *          中使用默认文本色。当前选择不可见时不自动改写其 ID，仅类别变化按规则清空选择。
     * @warning 搜索缓冲区和选择状态只由 GUI 线程访问。
     */
    static auto render_passive_skill_picker(PalworldEditorMod* self,
                                            const std::vector<skill_editor::SkillOption>& options,
                                            const std::unordered_set<std::string>& excludedIds)
        -> bool {
        const auto& selected = self->passivePickerState_.selected;
        const std::string preview =
            selected.has_value() ? skill_editor::skill_label(*selected) : "请选择技能";
        const bool coloredPreview = selected.has_value() && selected->passiveMetadata.has_value();
        if (coloredPreview) {
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  passive_category_color(selected->passiveMetadata->category));
        }
        bool changed = false;
        if (ImGui::BeginCombo("##passive-skill-picker", preview.c_str())) {
            ImGui::SetNextItemWidth(340.0F);
            ImGui::InputText("搜索##passive-skill-search", self->passiveSearch_,
                             sizeof(self->passiveSearch_));
            const auto visible = skill_editor::filter_passive_skill_views(
                options, self->passivePickerState_.category, self->passiveSearch_, excludedIds);
            for (const auto* option : visible) {
                const auto label = skill_editor::skill_label(*option);
                const bool isSelected = selected.has_value() && selected->id == option->id;
                if (option->passiveMetadata.has_value()) {
                    ImGui::PushStyleColor(
                        ImGuiCol_Text, passive_category_color(option->passiveMetadata->category));
                }
                if (ImGui::Selectable(label.c_str(), isSelected)) {
                    self->passivePickerState_.selected = *option;
                    changed = true;
                }
                if (option->passiveMetadata.has_value()) {
                    ImGui::PopStyleColor();
                }
            }
            ImGui::EndCombo();
        }
        if (coloredPreview) {
            ImGui::PopStyleColor();
        }
        return changed;
    }

    /**
     * @brief 渲染被动技能列表及新增、替换、删除工作流。
     * @param[in,out] self 非空、非拥有的当前 mod 实例指针。
     * @param[in] snapshot 当前技能目标、目录和实际状态的值快照。
     * @param[in] mutationsDisabled 是否应禁用全部被动技能修改入口。
     * @details 删除请求立即进入 FIFO；新增和替换先进入选择状态，确认后使用 Raw ID 提交。
     * @warning 只在 GUI 线程调用。
     */
    static void render_passive_skills(PalworldEditorMod* self, const SkillEditorSnapshot& snapshot,
                                      const bool mutationsDisabled) {
        const auto presets = skill_editor::passive_skill_presets();
        const bool presetSelectionValid =
            self->passivePresetIndex_.has_value() && *self->passivePresetIndex_ < presets.size();
        const auto presetPreview = presetSelectionValid
                                       ? presets[*self->passivePresetIndex_].displayName
                                       : std::string_view{"请选择词条预设"};

        ImGui::BeginDisabled(mutationsDisabled);
        if (ImGui::BeginCombo("词条预设##passive-preset", presetPreview.data())) {
            for (std::size_t index{}; index < presets.size(); ++index) {
                const bool selected =
                    self->passivePresetIndex_.has_value() && *self->passivePresetIndex_ == index;
                if (ImGui::Selectable(presets[index].displayName.data(), selected)) {
                    self->passivePresetIndex_ = index;
                }
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        if (presetSelectionValid) {
            const auto& preset = presets[*self->passivePresetIndex_];
            for (const auto id : preset.passiveIds) {
                const auto label = find_skill_label(snapshot.catalog.passive.skills, id);
                ImGui::BulletText("%s", label.c_str());
            }
        }

        ImGui::BeginDisabled(!presetSelectionValid);
        if (ImGui::Button("应用预设")) {
            self->skillQueue_.push(skill_editor::make_passive_preset_request(
                presets[*self->passivePresetIndex_], snapshot.targetGeneration,
                snapshot.worldGeneration));
            self->passiveEditIndex_ = -1;
            self->passivePickerState_.clear_selection();
        }
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::Separator();

        ImGui::Text("被动技能 (%d/4)", static_cast<int>(snapshot.state.passiveIds.size()));
        std::unordered_set<std::string> excluded(snapshot.state.passiveIds.begin(),
                                                 snapshot.state.passiveIds.end());

        ImGui::BeginDisabled(mutationsDisabled);
        for (std::size_t index = 0; index < snapshot.state.passiveIds.size(); ++index) {
            const auto& id = snapshot.state.passiveIds[index];
            const auto* option = find_skill_option(snapshot.catalog.passive.skills, id);
            const auto label =
                option != nullptr ? skill_editor::skill_label(*option) : std::string(id);
            if (option != nullptr && option->passiveMetadata.has_value()) {
                ImGui::TextColored(passive_category_color(option->passiveMetadata->category),
                                   "%d. %s", static_cast<int>(index + 1), label.c_str());
            } else {
                ImGui::Text("%d. %s", static_cast<int>(index + 1), label.c_str());
            }
            ImGui::SameLine();
            const auto replaceId = "替换##passive-" + std::to_string(index);
            if (ImGui::Button(replaceId.c_str())) {
                self->passiveEditIndex_ = static_cast<int>(index);
                self->passivePickerState_.clear_selection();
            }
            ImGui::SameLine();
            const auto removeId = "删除##passive-" + std::to_string(index);
            if (ImGui::Button(removeId.c_str())) {
                self->skillQueue_.push({.targetGeneration = snapshot.targetGeneration,
                                        .worldGeneration = snapshot.worldGeneration,
                                        .kind = skill_editor::SkillKind::passive,
                                        .operation = skill_editor::SkillEditOperation::remove,
                                        .oldPassiveId = id});
                self->passiveEditIndex_ = -1;
                self->passivePickerState_.clear_selection();
            }
        }

        if (snapshot.state.passiveIds.empty()) {
            ImGui::TextDisabled("暂无被动技能");
        }
        if (snapshot.state.passiveIds.size() < 4 && ImGui::Button("新增被动技能")) {
            self->passiveEditIndex_ = -2;
            self->passivePickerState_.clear_selection();
        }
        ImGui::EndDisabled();

        if (self->passiveEditIndex_ == -1) {
            return;
        }

        const bool replacing = self->passiveEditIndex_ >= 0;
        ImGui::TextUnformatted(replacing ? "选择替换后的被动技能：" : "选择要新增的被动技能：");
        ImGui::BeginDisabled(mutationsDisabled || !snapshot.catalog.passive.ready);
        render_passive_category_picker(self, snapshot.catalog);
        render_passive_skill_picker(self, snapshot.catalog.passive.skills, excluded);
        const bool canConfirm =
            self->passivePickerState_.selected.has_value() &&
            (!replacing ||
             self->passiveEditIndex_ < static_cast<int>(snapshot.state.passiveIds.size()));
        ImGui::BeginDisabled(!canConfirm);
        if (ImGui::Button("确认被动技能修改")) {
            skill_editor::SkillEditRequest request{
                .targetGeneration = snapshot.targetGeneration,
                .worldGeneration = snapshot.worldGeneration,
                .kind = skill_editor::SkillKind::passive,
                .operation = replacing ? skill_editor::SkillEditOperation::replace
                                       : skill_editor::SkillEditOperation::add,
                .newPassiveId = self->passivePickerState_.selected->id,
            };
            if (replacing) {
                request.oldPassiveId =
                    snapshot.state.passiveIds[static_cast<std::size_t>(self->passiveEditIndex_)];
            }
            self->skillQueue_.push(std::move(request));
            self->passiveEditIndex_ = -1;
            self->passivePickerState_.clear_selection();
        }
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("取消##passive")) {
            self->passiveEditIndex_ = -1;
            self->passivePickerState_.clear_selection();
        }
    }

    /**
     * @brief 渲染三个 `EquipWaza` 主动技能槽及装备、替换、清空工作流。
     * @param[in,out] self 非空、非拥有的当前 mod 实例指针。
     * @param[in] snapshot 当前技能目标、目录和实际状态的值快照。
     * @param[in] mutationsDisabled 是否应禁用全部主动技能修改入口。
     * @details 新技能只能追加到第一个尾部空槽；提交时同时携带 Raw ID 和 `EPalWazaID` 数值。
     * @warning 只在 GUI 线程调用。
     */
    static void render_active_skills(PalworldEditorMod* self, const SkillEditorSnapshot& snapshot,
                                     const bool mutationsDisabled) {
        ImGui::TextUnformatted("主动技能（EquipWaza）");
        std::unordered_set<std::string> excluded;
        for (const auto& skill : snapshot.state.activeSkills) {
            excluded.insert(skill.id);
        }

        ImGui::BeginDisabled(mutationsDisabled);
        for (std::size_t slot = 0; slot < 3; ++slot) {
            if (slot < snapshot.state.activeSkills.size()) {
                const auto& skill = snapshot.state.activeSkills[slot];
                const auto label = find_skill_label(snapshot.catalog.active.skills, skill.id);
                ImGui::Text("槽位 %d：%s", static_cast<int>(slot + 1), label.c_str());
                ImGui::SameLine();
                const auto replaceId = "替换##active-" + std::to_string(slot);
                if (ImGui::Button(replaceId.c_str())) {
                    self->activeEditSlot_ = static_cast<int>(slot);
                    self->activeChoice_.reset();
                    self->activeSearch_[0] = '\0';
                }
                ImGui::SameLine();
                const auto clearId = "清空##active-" + std::to_string(slot);
                if (ImGui::Button(clearId.c_str())) {
                    self->skillQueue_.push({.targetGeneration = snapshot.targetGeneration,
                                            .worldGeneration = snapshot.worldGeneration,
                                            .kind = skill_editor::SkillKind::active,
                                            .operation = skill_editor::SkillEditOperation::remove,
                                            .activeSlot = slot});
                    self->activeEditSlot_ = -1;
                    self->activeChoice_.reset();
                }
            } else {
                ImGui::Text("槽位 %d：空", static_cast<int>(slot + 1));
                if (slot == snapshot.state.activeSkills.size()) {
                    ImGui::SameLine();
                    const auto equipId = "选择/装备##active-" + std::to_string(slot);
                    if (ImGui::Button(equipId.c_str())) {
                        self->activeEditSlot_ = static_cast<int>(slot);
                        self->activeChoice_.reset();
                        self->activeSearch_[0] = '\0';
                    }
                }
            }
        }
        ImGui::EndDisabled();

        if (self->activeEditSlot_ < 0) {
            return;
        }

        const auto slot = static_cast<std::size_t>(self->activeEditSlot_);
        const bool replacing = slot < snapshot.state.activeSkills.size();
        ImGui::Text("为槽位 %d 选择主动技能：", self->activeEditSlot_ + 1);
        ImGui::BeginDisabled(mutationsDisabled || !snapshot.catalog.active.ready);
        render_skill_picker("##active-picker", snapshot.catalog.active.skills, excluded,
                            self->activeSearch_, sizeof(self->activeSearch_), self->activeChoice_);
        const bool canConfirm =
            self->activeChoice_.has_value() && self->activeChoice_->activeValue.has_value();
        ImGui::BeginDisabled(!canConfirm);
        if (ImGui::Button("确认主动技能修改")) {
            self->skillQueue_.push(
                {.targetGeneration = snapshot.targetGeneration,
                 .worldGeneration = snapshot.worldGeneration,
                 .kind = skill_editor::SkillKind::active,
                 .operation = replacing ? skill_editor::SkillEditOperation::replace
                                        : skill_editor::SkillEditOperation::add,
                 .activeSlot = slot,
                 .newActiveSkill = skill_editor::ActiveSkill{
                     .value = *self->activeChoice_->activeValue, .id = self->activeChoice_->id}});
            self->activeEditSlot_ = -1;
            self->activeChoice_.reset();
        }
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("取消##active")) {
            self->activeEditSlot_ = -1;
            self->activeChoice_.reset();
        }
    }

    /** @brief 渲染同公会跨据点制作/建造材料共享开关与运行状态。 */
    static void render_base_resource_sharing(PalworldEditorMod* self) {
        if (!ImGui::CollapsingHeader("据点资源共享")) {
            return;
        }

        const auto snapshot = self->baseResourceBridge_.snapshot();
        bool enabled = self->requestedBaseSharingEnabled_.load(std::memory_order_acquire);
        if (ImGui::Checkbox("同公会跨据点资源共享", &enabled)) {
            self->requestedBaseSharingEnabled_.store(enabled, std::memory_order_release);
            self->baseSharingSettingDirty_.store(true, std::memory_order_release);
        }

        ImGui::TextWrapped("%s", snapshot.status.c_str());
        const auto phaseLabel = [&snapshot]() -> const char* {
            switch (snapshot.persistentPhase) {
                case base_resource_sharing::PersistentUnionPhase::off:
                    return "关闭";
                case base_resource_sharing::PersistentUnionPhase::initializing:
                    return "初始化";
                case base_resource_sharing::PersistentUnionPhase::ready:
                    return "就绪";
                case base_resource_sharing::PersistentUnionPhase::reconciling:
                    return "校准";
                case base_resource_sharing::PersistentUnionPhase::restoring:
                    return "恢复";
                case base_resource_sharing::PersistentUnionPhase::failed:
                    return "安全停用";
            }
            return "未知";
        }();
        const auto surfaceLabel = [&snapshot]() -> const char* {
            switch (snapshot.consumerSurface) {
                case base_resource_sharing::ResourceConsumerSurface::none:
                    return "无";
                case base_resource_sharing::ResourceConsumerSurface::playerHelper:
                    return "玩家主背包 Helper";
                case base_resource_sharing::ResourceConsumerSurface::currentBaseModule:
                    return "当前据点仓储模块";
                case base_resource_sharing::ResourceConsumerSurface::guildBaseModules:
                    return "同公会据点仓储图";
            }
            return "未知";
        }();
        ImGui::TextDisabled("持久联合：%s；消费入口：%s", phaseLabel, surfaceLabel);
        ImGui::TextDisabled("原生登记边（已应用/待处理）：%zu / %zu", snapshot.appliedEdgeCount,
                            snapshot.pendingEdgeCount);
        ImGui::TextDisabled("可用/待加载容器：%zu / %zu", snapshot.containerCount,
                            snapshot.pendingContainerCount);
        ImGui::TextDisabled(
            "目录耗时（最近/最近成功/本世界峰值）：%.2f / %.2f / %.2f ms；尝试：%zu 次",
            snapshot.lastCatalogMilliseconds, snapshot.lastSuccessfulCatalogMilliseconds,
            snapshot.maximumCatalogMilliseconds, snapshot.catalogAttemptCount);
        if (snapshot.safetyDisabled) {
            ImGui::TextColored(
                ImVec4(1.0F, 0.35F, 0.2F, 1.0F),
                "检测到联合序列或恢复异常；相关能力已在本世界安全停用，切换开关不会绕过。");
        }
        ImGui::TextDisabled("仅本次游戏进程有效；重新启动游戏后默认关闭。");
        ImGui::TextDisabled("仅支持单人世界/本地房主；只影响制作和建造材料消耗，不合并箱子界面。");
    }

    /** @brief 渲染爪钩枪无冷却开关；切换时向游戏线程提交一次进程内请求。 */
    static void render_grapple_no_cooldown(PalworldEditorMod* self) {
        bool enabled = self->requestedGrappleNoCooldown_.load(std::memory_order_acquire);
        const auto phase = self->grappleRuntimePhase_.load(std::memory_order_acquire);
        const auto safetyDisabled = self->grappleSafetyDisabled_.load(std::memory_order_acquire) ||
                                    phase == grappling_hook::CooldownRuntimePhase::safetyDisabled;
        ImGui::BeginDisabled(safetyDisabled);
        if (ImGui::Checkbox("爪钩枪无冷却", &enabled)) {
            self->requestedGrappleNoCooldown_.store(enabled, std::memory_order_release);
            self->grappleSettingDirty_.store(true, std::memory_order_release);
        }
        ImGui::EndDisabled();
        if (phase == grappling_hook::CooldownRuntimePhase::waitingForRetry &&
            ImGui::Button("重新检测爪钩枪")) {
            self->grappleRetryRequested_.store(true, std::memory_order_release);
        }
        std::string runtimeStatus;
        {
            const std::lock_guard lock(self->grappleStatusMutex_);
            runtimeStatus = self->grappleRuntimeStatus_;
        }
        if (!runtimeStatus.empty()) {
            ImGui::TextWrapped("%s", runtimeStatus.c_str());
        }
        if (phase == grappling_hook::CooldownRuntimePhase::waitingForRetry) {
            ImGui::TextColored(ImVec4(1.0F, 0.75F, 0.2F, 1.0F),
                               "当前未找到已加载的正式爪钩枪；装备后请点击“重新检测爪钩枪”。");
        }
        if (safetyDisabled) {
            ImGui::TextColored(ImVec4(1.0F, 0.35F, 0.2F, 1.0F),
                               "爪钩字段布局、写入验证或恢复失败；已安全停用，切换开关不会绕过。");
        }
        ImGui::TextDisabled("仅本次游戏进程有效；重新启动游戏后默认关闭。");
        ImGui::TextDisabled(
            "只修改物品 ID 可确认的正式爪钩枪；关闭和切图时恢复原值。热卸载前请先关闭。");
    }

    /**
     * @brief 渲染等级、个体值与亲密度的编辑区，点击应用后入队一次属性请求。
     * @param[in,out] self 非空、非拥有的当前 mod 实例指针。
     * @param[in] snapshot 当前技能/属性编辑快照。
     * @param[in] mutationsDisabled 是否应禁用全部属性修改入口。
     * @warning 只在 GUI 线程调用。
     */
    static void render_pal_stats(PalworldEditorMod* self, const SkillEditorSnapshot& snapshot,
                                 const bool mutationsDisabled) {
        ImGui::TextUnformatted("属性修改");
        const auto& stats = snapshot.palStat;
        if (!stats.readable) {
            ImGui::TextDisabled("属性读取中或不可用");
            return;
        }
        self->statDraft_.reconcile(stats, snapshot.targetGeneration);
        auto& values = self->statDraft_.values();
        ImGui::Text("当前：等级 %d / HP %d / 攻击 %d / 防御 %d / 亲密度 %d", stats.level,
                    stats.talentHp, stats.talentShot, stats.talentDefense, stats.friendshipRank);

        ImGui::SetNextItemWidth(120.0F);
        ImGui::DragInt("等级##stat-level", &values.level, 1.0F, pal_stats::kLevelMin,
                       pal_stats::kLevelMax);
        ImGui::SetNextItemWidth(120.0F);
        ImGui::DragInt("个体值·HP##stat-hp", &values.talentHp, 1.0F, pal_stats::kTalentMin,
                       pal_stats::kTalentMax);
        ImGui::SetNextItemWidth(120.0F);
        ImGui::DragInt("个体值·攻击##stat-atk", &values.talentShot, 1.0F, pal_stats::kTalentMin,
                       pal_stats::kTalentMax);
        ImGui::SetNextItemWidth(120.0F);
        ImGui::DragInt("个体值·防御##stat-def", &values.talentDefense, 1.0F, pal_stats::kTalentMin,
                       pal_stats::kTalentMax);
        ImGui::SetNextItemWidth(120.0F);
        ImGui::DragInt("亲密度##stat-friend", &values.friendshipRank, 1.0F,
                       pal_stats::kFriendshipRankMin, pal_stats::kFriendshipRankMax);

        const auto request = self->statDraft_.make_request(snapshot.worldGeneration);
        ImGui::BeginDisabled(mutationsDisabled || !request.has_value());
        if (ImGui::Button("应用属性修改")) {
            self->statRequestSlot_.submit(*request);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("(写入存档；重新召唤或重载后面板刷新)");
    }

    /**
     * @brief 渲染当前待出战帕鲁、技能目录状态和主动/被动技能编辑区域。
     * @param[in,out] self 非空、非拥有的当前 mod 实例指针。
     * @details 空闲时不执行后台解析；选择和编辑请求会在当次游戏线程回调立即解析并校验目标。
     *          用户点击“选择当前帕鲁”后才显示编辑区，只有再次点击才会切换锁定目标。
     *          GUI 请求只携带目标代数，不传递 Unreal 对象地址。
     * @warning 只在 GUI 线程调用。
     */
    static void render_pal_editor(PalworldEditorMod* self) {
        if (!ImGui::CollapsingHeader("Pal editor")) {
            return;
        }

        SkillEditorSnapshot snapshot;
        {
            const std::lock_guard lock(self->skillSnapshotMutex_);
            snapshot = self->skillSnapshot_;
        }
        if (self->skillUiGeneration_ != snapshot.targetGeneration ||
            self->skillUiWorldGeneration_ != snapshot.worldGeneration) {
            self->skillUiGeneration_ = snapshot.targetGeneration;
            self->skillUiWorldGeneration_ = snapshot.worldGeneration;
            reset_skill_editor_ui(self);
        }
        const auto choiceStillExists = [](const std::optional<skill_editor::SkillOption>& choice,
                                          const skill_editor::SkillCatalogSection& section) {
            return !choice.has_value() ||
                   std::ranges::any_of(section.skills, [&choice](const auto& option) {
                       return option.id == choice->id;
                   });
        };
        if (!choiceStillExists(self->passivePickerState_.selected, snapshot.catalog.passive)) {
            self->passivePickerState_.clear_selection();
        }
        if (!choiceStillExists(self->activeChoice_, snapshot.catalog.active)) {
            self->activeChoice_.reset();
        }

        bool selectionPending = false;
        {
            const std::lock_guard lock(self->selectionRequestMutex_);
            selectionPending = self->selectCurrentPalRequest_.has_value();
        }
        const bool pending = snapshot.pending || self->skillQueue_.size() != 0 ||
                             self->statRequestSlot_.has_pending() || selectionPending;
        const bool catalogReady = skill_editor::catalog_is_ready_for_editing(snapshot.catalog);
        const bool lifecycleReady =
            snapshot.worldAccessible && snapshot.worldLifecycleCallbacksReady;
        const bool editingReady = lifecycleReady && snapshot.targetMatchesCurrent && catalogReady;
        ImGui::BeginDisabled(pending || !lifecycleReady);
        if (ImGui::Button("选择当前帕鲁")) {
            const std::lock_guard lock(self->selectionRequestMutex_);
            self->selectCurrentPalRequest_ = skill_editor::WorldBoundRequest{
                .worldGeneration = snapshot.worldGeneration,
            };
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(pending || !snapshot.worldAccessible);
        if (ImGui::Button("刷新技能列表")) {
            self->wantRefreshSkillCatalog_.store(true);
        }
        ImGui::EndDisabled();

        if (snapshot.targetSelected) {
            ImGui::TextColored(ImVec4(0.4F, 1.0F, 0.4F, 1.0F), "当前已选择帕鲁：%s",
                               snapshot.palName.empty() ? "(读取中...)" : snapshot.palName.c_str());
        } else {
            ImGui::TextDisabled(
                "请用数字键高亮队伍帕鲁，再点击“选择当前帕鲁”；目标应与下一次按 E 召唤一致。");
        }
        if (snapshot.targetConfirmedForWorld &&
            snapshot.resolutionStatus != skill_editor::SelectedTargetResolutionStatus::success) {
            const auto message = skill_editor::resolution_status_message(snapshot.resolutionStatus);
            ImGui::TextColored(ImVec4(1.0F, 0.45F, 0.35F, 1.0F), "解析状态：%.*s",
                               static_cast<int>(message.size()), message.data());
        }
        if (!catalogReady) {
            ImGui::TextColored(ImVec4(1.0F, 0.8F, 0.2F, 1.0F),
                               "技能目录正在等待游戏初始化，将每 2 秒自动重试；"
                               "就绪前仅可查看技能。");
        }
        if (snapshot.targetSelected && !snapshot.targetMatchesCurrent) {
            const char* message = "暂时无法确认当前高亮目标；已保留选择并暂停技能修改。";
            if (!snapshot.targetConfirmedForWorld) {
                message = "世界已切换；已保留原选择用于显示，请重新点击“选择当前帕鲁”后再修改。";
            } else if (snapshot.resolutionStatus ==
                       skill_editor::SelectedTargetResolutionStatus::success) {
                message = "当前数字键高亮帕鲁与已选择目标不同；点击“选择当前帕鲁”后才会切换。";
            }
            ImGui::TextColored(ImVec4(1.0F, 0.8F, 0.2F, 1.0F), "%s", message);
        }
        if (!snapshot.worldLifecycleCallbacksReady) {
            ImGui::TextColored(ImVec4(1.0F, 0.45F, 0.35F, 1.0F),
                               "UE4SS 世界切换回调不可用；为防止存档/切图崩溃，技能编辑已停用。");
        } else if (!snapshot.worldAccessible) {
            ImGui::TextColored(ImVec4(1.0F, 0.8F, 0.2F, 1.0F),
                               "正在切换世界；所有待处理修改均已取消。");
        }
        if (snapshot.statWritesDisabled) {
            ImGui::TextColored(ImVec4(1.0F, 0.35F, 0.2F, 1.0F),
                               "属性写入已在本世界安全停用；请退出并重新进入世界。");
        }
        if (pending) {
            ImGui::TextColored(ImVec4(1.0F, 0.8F, 0.2F, 1.0F), "技能修改处理中...");
        }
        if (!snapshot.lastResult.empty()) {
            ImGui::TextWrapped("结果：%s", snapshot.lastResult.c_str());
        }
        if (!snapshot.catalog.passive.error.empty()) {
            ImGui::TextColored(ImVec4(1.0F, 0.45F, 0.35F, 1.0F), "被动技能目录：%s",
                               snapshot.catalog.passive.error.c_str());
        }
        if (!snapshot.catalog.active.error.empty()) {
            ImGui::TextColored(ImVec4(1.0F, 0.45F, 0.35F, 1.0F), "主动技能目录：%s",
                               snapshot.catalog.active.error.c_str());
        }
        ImGui::Separator();

        if (!snapshot.targetSelected) {
            return;
        }

        ImGui::Separator();
        render_passive_skills(self, snapshot, pending || !editingReady);
        ImGui::Separator();
        render_active_skills(self, snapshot, pending || !editingReady);
        ImGui::Separator();
        render_pal_stats(self, snapshot, pending || !editingReady || snapshot.statWritesDisabled);
    }

    /** @brief 给予物品输入框中的 ASCII Raw ID；只由 GUI 线程访问。 */
    char item_buf_[64] = "PalSphere_Tera";
    /** @brief 物品目录搜索缓冲区；只由 GUI 线程访问。 */
    char search_buf_[64]{};
    /** @brief 给予物品数量输入值；只由 GUI 线程访问并限制到 `[1, 9999]`。 */
    int count_input_ = 10;
    /** @brief 背包槽位新数量输入值；只由 GUI 线程访问并限制到 `[0, 9999]`。 */
    int set_count_input_ = 0;
    /** @brief GUI 当前选择的背包快照索引；`-1` 表示未选择。 */
    int selected_ = -1;

    /**
     * @brief 保护给予物品和修改槽位的复合请求参数。
     * @details GUI 线程写入参数，游戏线程在 EngineTick 回调中复制参数。
     */
    std::mutex req_mutex_;
    /** @brief 待给予的物品 Raw ID；由 req_mutex_ 保护。 */
    std::string give_item_;
    /** @brief 待给予的物品数量；由 req_mutex_ 保护。 */
    int give_count_ = 0;
    /** @brief GUI 向游戏线程发布给予请求的原子标志。 */
    std::atomic<bool> give_requested_{false};
    /** @brief 待修改的主背包槽位索引；由 req_mutex_ 保护。 */
    int32_t modify_slot_ = 0;
    /** @brief 待写入槽位的堆叠数量；由 req_mutex_ 保护。 */
    int32_t modify_count_ = 0;
    /** @brief GUI 向游戏线程发布槽位数量修改请求的原子标志。 */
    std::atomic<bool> modify_requested_{false};

    /**
     * @brief 保护背包和物品目录值快照。
     * @details 游戏线程写入这些快照，GUI 线程读取并更新对应选择索引。
     */
    std::mutex inv_mutex_;
    /** @brief 最近一次游戏线程读取的主背包非空槽位快照；由 inv_mutex_ 保护。 */
    std::vector<InvEntry> inv_cache_;
    /** @brief 最近一次物品扫描生成的本地化目录快照；由 inv_mutex_ 保护。 */
    item_catalog::ItemCatalogSnapshot item_db_cache_;

    /** @brief 请求游戏线程在下一次更新中刷新主背包快照。 */
    std::atomic<bool> want_read_{false};
    /** @brief 请求游戏线程在下一次更新中输出 UObject 诊断信息。 */
    std::atomic<bool> want_discover_{false};
    /** @brief 请求游戏线程在下一次更新中重新扫描物品目录。 */
    std::atomic<bool> want_scan_items_{false};
    /** @brief 主数据未就绪时按世界进行有界低频补全，不访问 Unreal。 */
    item_catalog::ItemCatalogScanScheduler itemCatalogScanScheduler_;
    /** @brief 请求首次 EngineTick 输出 UObject 诊断信息。 */
    std::atomic<bool> wantProbeObject_{false};

    /** @brief 游戏线程拥有的跨据点资源反射桥；GUI 只读取其值快照。 */
    base_resource_sharing::PalBaseResourceBridge baseResourceBridge_;
    /** @brief GUI/启动阶段提交给游戏线程的资源共享偏好。 */
    std::atomic<bool> requestedBaseSharingEnabled_{false};
    /** @brief 通知 EngineTick 消费最新资源共享偏好。 */
    std::atomic<bool> baseSharingSettingDirty_{false};
    /** @brief 用户期望的爪钩枪无冷却偏好；GUI 写入、EngineTick 消费。 */
    std::atomic<bool> requestedGrappleNoCooldown_{false};
    /** @brief 通知 EngineTick 更新爪钩覆盖领域服务的期望状态。 */
    std::atomic<bool> grappleSettingDirty_{false};
    /** @brief GUI 显式授权目标未加载后的下一次单次检测。 */
    std::atomic<bool> grappleRetryRequested_{false};
    /** @brief 只在游戏线程执行严格目标识别、冷却覆盖和恢复的反射网关。 */
    grappling_hook::GrappleCooldownGateway grappleGateway_;
    /** @brief 只保存对象全名、原值和世界代次的可逆覆盖领域状态。 */
    grappling_hook::CooldownOverrideLedger grappleLedger_;
    /** @brief 有界调度玩家就绪检查，避免等待背包时逐帧解析。 */
    grappling_hook::CooldownReadinessScheduler grappleReadinessScheduler_;
    /** @brief 游戏线程发布、GUI 只读的爪钩领域阶段。 */
    std::atomic<grappling_hook::CooldownRuntimePhase> grappleRuntimePhase_{
        grappling_hook::CooldownRuntimePhase::off};
    /** @brief 恢复验证失败后阻止本次运行继续建立爪钩覆盖。 */
    std::atomic<bool> grappleSafetyDisabled_{false};
    /** @brief 保护游戏线程发布、GUI 复制的爪钩运行时状态。 */
    std::mutex grappleStatusMutex_;
    /** @brief 最近一次爪钩应用或恢复结果的面向用户文本。 */
    std::string grappleRuntimeStatus_;

    /** @brief 在游戏线程执行 Palworld 技能反射读写的无 UObject 所有权网关。 */
    pal_skills::PalSkillGateway skillGateway_;
    /** @brief 在游戏线程执行帕鲁属性反射读写的无 UObject 所有权网关。 */
    pal_stats::PalStatGateway statGateway_;
    /** @brief 属性恢复验证失败后阻止本世界继续写入；仅由游戏线程访问。 */
    bool statWritesDisabledForWorld_{};
    /** @brief GUI 生产、游戏线程消费且只保留最新状态的线程安全属性请求槽。 */
    pal_stats::PalStatEditRequestSlot statRequestSlot_;
    /** @brief 仅由 EngineTick/LoadMap 游戏线程回调访问的世界代次与确认状态。 */
    skill_editor::WorldSessionState worldSession_;
    /** @brief 游戏线程保存的、由用户显式确认的下一次按 E 召唤帕鲁纯值目标状态。 */
    skill_editor::SelectedTargetState selectedTarget_;
    /** @brief 最近一次解析结果的纯值副本；不含任何 Unreal 指针。 */
    skill_editor::TargetResolutionState targetResolutionState_;
    /** @brief 最近一次输出日志的目标解析状态；仅由游戏线程访问。 */
    std::optional<skill_editor::SelectedTargetResolutionStatus> lastResolutionStatus_;
    /** @brief GUI 生产、游戏线程 FIFO 消费的线程安全技能编辑请求队列。 */
    skill_editor::SkillEditQueue skillQueue_;
    /** @brief 保护游戏线程发布、GUI 线程复制的 skillSnapshot_。 */
    std::mutex skillSnapshotMutex_;
    /** @brief 仅由游戏线程修改的技能编辑工作快照。 */
    SkillEditorSnapshot skillRuntimeSnapshot_;
    /** @brief 最近一次发布给 GUI 的完整技能编辑快照；由 skillSnapshotMutex_ 保护。 */
    SkillEditorSnapshot skillSnapshot_;
    /** @brief 游戏线程工作快照是否包含尚未发布的可观察变化。 */
    bool skillSnapshotDirty_{true};
    /** @brief 保护绑定世界代次的“选择当前帕鲁”请求。 */
    std::mutex selectionRequestMutex_;
    /** @brief GUI 提交、EngineTick 消费的最新目标选择请求。 */
    std::optional<skill_editor::WorldBoundRequest> selectCurrentPalRequest_;
    /** @brief 请求游戏线程立即重新加载完整技能目录。 */
    std::atomic<bool> wantRefreshSkillCatalog_{false};
    /** @brief 启动阶段每两秒重试一次完整技能目录加载。 */
    skill_editor::SkillCatalogRefreshScheduler skillCatalogRefreshScheduler_{
        std::chrono::seconds{2}};
    /** @brief 跨 EngineTick 但不含 Unreal 指针的被动技能分类任务。 */
    skill_editor::PassiveSkillClassificationJob passiveClassificationJob_;
    /** @brief mod 生命周期内成功读取的被动技能分类纯值缓存。 */
    std::unordered_map<std::string, skill_editor::PassiveSkillMetadata> passiveSkillMetadataCache_;
    /** @brief GUI 可无锁读取的当前分类完成数。 */
    std::atomic<std::size_t> passiveClassificationCompleted_{0};
    /** @brief GUI 可无锁读取的当前分类总数。 */
    std::atomic<std::size_t> passiveClassificationTotal_{0};
    /** @brief 本轮刷新前是否已有可供失败回退的具体类别快照。 */
    bool hadUsablePassiveClassificationBeforeRefresh_{};
    /** @brief 本轮所有分类批次累计占用的游戏线程时间。 */
    std::chrono::microseconds passiveClassificationElapsed_{};
    /** @brief 本轮分类实际消耗的 EngineTick 数。 */
    std::size_t passiveClassificationTicks_{};
    /** @brief 被动技能下拉框搜索缓冲区；只由 GUI 线程访问。 */
    char passiveSearch_[96]{};
    /** @brief 主动技能下拉框搜索缓冲区；只由 GUI 线程访问。 */
    char activeSearch_[96]{};
    /** @brief 从已确认目标真实快照初始化、只生成差量请求的属性编辑草稿。 */
    pal_stats::PalStatEditDraft statDraft_;
    /**
     * @brief 被动技能编辑模式与索引；只由 GUI 线程访问。
     * @details `-1` 表示未编辑，`-2` 表示新增，非负值表示要替换的被动技能索引。
     */
    int passiveEditIndex_ = -1;
    /** @brief 主动技能编辑槽位；`-1` 表示未编辑，非负值表示 `EquipWaza` 槽位。 */
    int activeEditSlot_ = -1;
    /** @brief 被动技能两级选择器的类别与当前选择；只由 GUI 线程访问。 */
    skill_editor::PassiveSkillPickerState passivePickerState_;
    /** @brief 主动技能下拉框当前选择的目录值；只由 GUI 线程访问。 */
    std::optional<skill_editor::SkillOption> activeChoice_;
    /** @brief 词条预设下拉框当前选择的静态目录索引；只由 GUI 线程访问。 */
    std::optional<std::size_t> passivePresetIndex_;
    /** @brief GUI 上一次渲染的目标代数；变化时重置临时编辑状态。 */
    std::uint64_t skillUiGeneration_{};
    /** @brief GUI 上一次渲染的世界代次；变化时重置预设和临时编辑状态。 */
    std::uint64_t skillUiWorldGeneration_{};

    /** @brief EngineTick 游戏线程回调 ID。 */
    Hook::GlobalCallbackId engineTickCallbackId_{Hook::ERROR_ID};
    /** @brief LoadMap 前置世界失效回调 ID。 */
    Hook::GlobalCallbackId loadMapPreCallbackId_{Hook::ERROR_ID};
    /** @brief LoadMap 后置世界恢复回调 ID。 */
    Hook::GlobalCallbackId loadMapPostCallbackId_{Hook::ERROR_ID};
    /** @brief 两个 LoadMap 生命周期回调是否均已成功注册。 */
    std::atomic<bool> worldLifecycleCallbacksReady_{false};
};

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
}
