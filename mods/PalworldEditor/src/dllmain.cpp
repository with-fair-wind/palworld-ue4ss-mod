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
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
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
#include <base_resource_sharing/settings.hpp>
#include <game/pal_game.hpp>
#include <imgui.h>
#include <items/item_catalog.hpp>
#include <skills/pal_resolution_scheduler.hpp>
#include <skills/pal_skills.hpp>
#include <skills/selected_target_state.hpp>
#include <skills/world_session_state.hpp>

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
        ModVersion = STR("1.6.1");
        ModDescription =
            STR("Item, Pal skill, and same-guild base resource editor for Palworld 1.0");
        ModAuthors = STR("with-fair-wind");

        Output::send<LogLevel::Verbose>(STR("PalworldEditor loaded (v1.6.1)\n"));

        register_tab(STR("PalworldEditor"), [](CppUserModBase* mod) {
            UE4SS_ENABLE_IMGUI()
            auto* self = static_cast<PalworldEditorMod*>(mod);
            ImGui::TextUnformatted("A floating 'PalworldEditor' window should be visible ->");
            if (ImGui::Begin("PalworldEditor v1.6.1", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                render_give_items(self);
                ImGui::Separator();
                render_item_browser(self);
                ImGui::Separator();
                render_inventory(self);
                ImGui::Separator();
                render_base_resource_sharing(self);
                ImGui::Separator();
                render_pal_editor(self);
                ImGui::Separator();
                if (ImGui::Button("Discover")) {
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

    /** @brief 从 mod 配置目录读取默认关闭的跨据点资源共享偏好。 */
    auto on_program_start() -> void override {
        configPath_ = std::filesystem::path{UE4SSProgram::get_program().get_mods_directory()} /
                      "PalworldEditor" / "config.ini";
        const auto loaded = base_resource_sharing::load_settings(configPath_);
        requestedBaseSharingEnabled_.store(loaded.settings.enabled);
        baseSharingSettingDirty_.store(true);
        baseResourceBridge_.set_config_error(loaded.error);
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

        // Scan items
        if (want_scan_items_.exchange(false)) {
            auto fresh = pal_game::scan_all_items();
            const std::lock_guard lock(inv_mutex_);
            item_db_cache_ = std::move(fresh);
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
        if (selectionRequested) {
            skillQueue_.clear();
        } else {
            editRequest = skillQueue_.try_pop();
        }

        const auto trigger = palResolutionScheduler_.decide(
            worldSession_.is_target_confirmed() && selectedTarget_.is_selected(),
            selectionRequested, editRequest.has_value(),
            skill_editor::PalResolutionScheduler::clock::now());
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
                skillRuntimeSnapshot_.lastResult.clear();
            } else {
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

        const bool manualRefreshRequested = wantRefreshSkillCatalog_.exchange(false);
        const bool catalogReady =
            skill_editor::catalog_is_ready_for_editing(skillRuntimeSnapshot_.catalog);
        const bool refreshRequested = skillCatalogRefreshScheduler_.should_refresh(
            manualRefreshRequested, catalogReady,
            skill_editor::SkillCatalogRefreshScheduler::clock::now(),
            [] { return pal_game::is_valid(pal_game::get_main_container()); });
        if (refreshRequested) {
            skillRuntimeSnapshot_.catalog = skill_editor::with_catalog_fallback(
                skillRuntimeSnapshot_.catalog, skillGateway_.load_catalog());
            skillSnapshotDirty_ = true;
        }

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

    /** @brief Invalidates all work and write authorization before Unreal replaces the world. */
    auto begin_world_transition() -> void {
        baseResourceBridge_.on_world_begin(worldSession_.generation() + 1);
        worldSession_.begin_transition();
        skillQueue_.clear();
        {
            const std::lock_guard lock(selectionRequestMutex_);
            selectCurrentPalRequest_.reset();
        }

        give_requested_.store(false);
        modify_requested_.store(false);
        want_read_.store(false);
        want_discover_.store(false);
        want_scan_items_.store(false);
        wantRefreshSkillCatalog_.store(false);
        wantProbeObject_.store(false);

        {
            const std::lock_guard lock(inv_mutex_);
            inv_cache_.clear();
            item_db_cache_ = {};
            selected_ = -1;
        }

        lastResolutionStatus_.reset();
        palResolutionScheduler_.reset();
        targetResolutionState_.reset();
        skillRuntimeSnapshot_.targetGeneration = selectedTarget_.generation();
        skillRuntimeSnapshot_.worldGeneration = worldSession_.generation();
        skillRuntimeSnapshot_.palName =
            selectedTarget_.is_selected() ? selectedTarget_.current().name : std::string{};
        skillRuntimeSnapshot_.state = {};
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
        want_read_.store(true);
        want_scan_items_.store(true);
        wantRefreshSkillCatalog_.store(true);

        palResolutionScheduler_.reset();
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
     * @brief 清空与上一个技能目标相关的 GUI 临时编辑状态。
     * @param[in,out] self 非空、非拥有的当前 mod 实例指针。
     * @warning 只在 GUI 线程调用。
     */
    static void reset_skill_editor_ui(PalworldEditorMod* self) {
        self->passiveEditIndex_ = -1;
        self->activeEditSlot_ = -1;
        self->passiveChoice_.reset();
        self->activeChoice_.reset();
        self->passiveSearch_[0] = '\0';
        self->activeSearch_[0] = '\0';
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
        ImGui::TextUnformatted("Give items");
        ImGui::InputText("Item ID", self->item_buf_, sizeof(self->item_buf_));
        ImGui::InputInt("Count", &self->count_input_);
        self->count_input_ = clamp(self->count_input_, 1, 9999);
        if (ImGui::Button("Give")) {
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
        if (ImGui::Button("Scan game items")) {
            self->want_scan_items_.store(true);
        }
        ImGui::SameLine();
        ImGui::InputText("##search", self->search_buf_, sizeof(self->search_buf_));
        {
            const std::lock_guard lock(self->inv_mutex_);
            ImGui::TextDisabled("(%d items)", static_cast<int>(self->item_db_cache_.items.size()));
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
        if (ImGui::Button("Refresh inventory")) {
            self->want_read_.store(true);
        }
        ImGui::SameLine();
        ImGui::TextUnformatted("(click an item to select, then set count)");
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
                ImGui::Text("Selected: %s (slot %d, x%d)", itemLabel.c_str(),
                            static_cast<int>(e.slot_index), e.count);
                ImGui::InputInt("New count", &self->set_count_input_);
                self->set_count_input_ = clamp(self->set_count_input_, 0, 9999);
                if (ImGui::Button("Set count")) {
                    const std::lock_guard lock2(self->req_mutex_);
                    self->modify_slot_ = e.slot_index;
                    self->modify_count_ = self->set_count_input_;
                    self->modify_requested_ = true;
                }
            }
        }
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
        ImGui::Text("被动技能 (%d/4)", static_cast<int>(snapshot.state.passiveIds.size()));
        std::unordered_set<std::string> excluded(snapshot.state.passiveIds.begin(),
                                                 snapshot.state.passiveIds.end());

        ImGui::BeginDisabled(mutationsDisabled);
        for (std::size_t index = 0; index < snapshot.state.passiveIds.size(); ++index) {
            const auto& id = snapshot.state.passiveIds[index];
            const auto label = find_skill_label(snapshot.catalog.passive.skills, id);
            ImGui::Text("%d. %s", static_cast<int>(index + 1), label.c_str());
            ImGui::SameLine();
            const auto replaceId = "替换##passive-" + std::to_string(index);
            if (ImGui::Button(replaceId.c_str())) {
                self->passiveEditIndex_ = static_cast<int>(index);
                self->passiveChoice_.reset();
                self->passiveSearch_[0] = '\0';
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
                self->passiveChoice_.reset();
            }
        }

        if (snapshot.state.passiveIds.empty()) {
            ImGui::TextDisabled("暂无被动技能");
        }
        if (snapshot.state.passiveIds.size() < 4 && ImGui::Button("新增被动技能")) {
            self->passiveEditIndex_ = -2;
            self->passiveChoice_.reset();
            self->passiveSearch_[0] = '\0';
        }
        ImGui::EndDisabled();

        if (self->passiveEditIndex_ == -1) {
            return;
        }

        const bool replacing = self->passiveEditIndex_ >= 0;
        ImGui::TextUnformatted(replacing ? "选择替换后的被动技能：" : "选择要新增的被动技能：");
        ImGui::BeginDisabled(mutationsDisabled || !snapshot.catalog.passive.ready);
        render_skill_picker("##passive-picker", snapshot.catalog.passive.skills, excluded,
                            self->passiveSearch_, sizeof(self->passiveSearch_),
                            self->passiveChoice_);
        const bool canConfirm =
            self->passiveChoice_.has_value() &&
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
                .newPassiveId = self->passiveChoice_->id,
            };
            if (replacing) {
                request.oldPassiveId =
                    snapshot.state.passiveIds[static_cast<std::size_t>(self->passiveEditIndex_)];
            }
            self->skillQueue_.push(std::move(request));
            self->passiveEditIndex_ = -1;
            self->passiveChoice_.reset();
        }
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("取消##passive")) {
            self->passiveEditIndex_ = -1;
            self->passiveChoice_.reset();
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
        bool enabled = self->requestedBaseSharingEnabled_.load();
        if (ImGui::Checkbox("同公会跨据点资源共享", &enabled)) {
            self->requestedBaseSharingEnabled_.store(enabled);
            self->baseSharingSettingDirty_.store(true);
            const auto error =
                self->configPath_.empty()
                    ? std::string{"配置路径尚未初始化，设置未持久化。"}
                    : base_resource_sharing::save_settings(
                          self->configPath_, base_resource_sharing::Settings{.enabled = enabled});
            self->baseResourceBridge_.set_config_error(error);
        }

        ImGui::TextWrapped("%s", snapshot.status.c_str());
        if (!snapshot.configError.empty()) {
            ImGui::TextColored(ImVec4(1.0F, 0.35F, 0.2F, 1.0F), "%s", snapshot.configError.c_str());
        }
        ImGui::TextDisabled("仅支持单人世界/本地房主；只影响制作和建造材料消耗，不合并箱子界面。");
    }

    /**
     * @brief 渲染当前待出战帕鲁、技能目录状态和主动/被动技能编辑区域。
     * @param[in,out] self 非空、非拥有的当前 mod 实例指针。
     * @details 未确认目标时不执行后台解析；确认后最多每 250 毫秒校验一次数字键当前高亮目标，
     *          选择和编辑请求仍会在写入前立即解析。
     *          用户点击“选择当前帕鲁”后才显示编辑区。
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
        if (self->skillUiGeneration_ != snapshot.targetGeneration) {
            self->skillUiGeneration_ = snapshot.targetGeneration;
            reset_skill_editor_ui(self);
        }
        const auto choiceStillExists = [](const std::optional<skill_editor::SkillOption>& choice,
                                          const skill_editor::SkillCatalogSection& section) {
            return !choice.has_value() ||
                   std::ranges::any_of(section.skills, [&choice](const auto& option) {
                       return option.id == choice->id;
                   });
        };
        if (!choiceStillExists(self->passiveChoice_, snapshot.catalog.passive)) {
            self->passiveChoice_.reset();
        }
        if (!choiceStillExists(self->activeChoice_, snapshot.catalog.active)) {
            self->activeChoice_.reset();
        }

        bool selectionPending = false;
        {
            const std::lock_guard lock(self->selectionRequestMutex_);
            selectionPending = self->selectCurrentPalRequest_.has_value();
        }
        const bool pending = snapshot.pending || self->skillQueue_.size() != 0 || selectionPending;
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
    /** @brief 请求首次 EngineTick 输出 UObject 诊断信息。 */
    std::atomic<bool> wantProbeObject_{false};

    /** @brief 游戏线程拥有的跨据点资源反射桥；GUI 只读取其值快照。 */
    base_resource_sharing::PalBaseResourceBridge baseResourceBridge_;
    /** @brief `ue4ss/Mods/PalworldEditor/config.ini` 的绝对路径。 */
    std::filesystem::path configPath_;
    /** @brief GUI/启动阶段提交给游戏线程的资源共享偏好。 */
    std::atomic<bool> requestedBaseSharingEnabled_{false};
    /** @brief 通知 EngineTick 消费最新资源共享偏好。 */
    std::atomic<bool> baseSharingSettingDirty_{false};

    /** @brief 在游戏线程执行 Palworld 技能反射读写的无 UObject 所有权网关。 */
    pal_skills::PalSkillGateway skillGateway_;
    /** @brief 仅由 EngineTick/LoadMap 游戏线程回调访问的世界代次与确认状态。 */
    skill_editor::WorldSessionState worldSession_;
    /** @brief 游戏线程保存的、由用户显式确认的下一次按 E 召唤帕鲁纯值目标状态。 */
    skill_editor::SelectedTargetState selectedTarget_;
    /** @brief 选择/编辑时立即解析、确认目标后每 250 毫秒校验一次的纯值调度器。 */
    skill_editor::PalResolutionScheduler palResolutionScheduler_;
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
    /** @brief 被动技能下拉框搜索缓冲区；只由 GUI 线程访问。 */
    char passiveSearch_[96]{};
    /** @brief 主动技能下拉框搜索缓冲区；只由 GUI 线程访问。 */
    char activeSearch_[96]{};
    /**
     * @brief 被动技能编辑模式与索引；只由 GUI 线程访问。
     * @details `-1` 表示未编辑，`-2` 表示新增，非负值表示要替换的被动技能索引。
     */
    int passiveEditIndex_ = -1;
    /** @brief 主动技能编辑槽位；`-1` 表示未编辑，非负值表示 `EquipWaza` 槽位。 */
    int activeEditSlot_ = -1;
    /** @brief 被动技能下拉框当前选择的目录值；只由 GUI 线程访问。 */
    std::optional<skill_editor::SkillOption> passiveChoice_;
    /** @brief 主动技能下拉框当前选择的目录值；只由 GUI 线程访问。 */
    std::optional<skill_editor::SkillOption> activeChoice_;
    /** @brief GUI 上一次渲染的目标代数；变化时重置临时编辑状态。 */
    std::uint64_t skillUiGeneration_{};

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
