/**
 * @file mod_core.hpp
 * @brief PalworldEditorMod 实例的类声明：GUI 状态、请求队列、值缓存与回调声明。
 * @details 类本身拥有 GUI 状态、请求队列和值类型缓存，但不拥有任何 Unreal UObject。
 *          成员函数实现分布在 src/mod/dllmain.cpp 与各功能的 *_ui.cpp 中；GUI 线程不得
 *          直接调用反射接口，游戏线程通过 EngineTick 回调消费请求并发布快照。
 */
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <Mod/CppUserModBase.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <base_resource_sharing/pal_base_resources.hpp>
#include <game/pal_game.hpp>
#include <grappling_hook/cooldown_gateway.hpp>
#include <imgui.h>
#include <items/item_catalog.hpp>
#include <pal_identity/pal_identity.hpp>
#include <pal_identity/pal_identity_editor.hpp>
#include <pal_remote_palbox/remote_palbox_runtime.hpp>
#include <pal_stats/pal_stat_editor.hpp>
#include <pal_stats/pal_stats.hpp>
#include <skills/pal_resolution_scheduler.hpp>
#include <skills/pal_skills.hpp>
#include <skills/selected_target_state.hpp>
#include <skills/skill_catalog.hpp>
#include <skills/skill_editor_service.hpp>
#include <skills/world_session_state.hpp>

/**
 * @brief PalworldEditor 的 UE4SS mod 实例与全部运行时状态容器。
 * @details 类本身拥有 GUI 状态、请求队列和值类型缓存，但不拥有任何 Unreal UObject。
 *          GUI 线程不得直接调用反射接口；游戏线程通过 EngineTick 回调消费请求并发布快照。
 */
class PalworldEditorMod final : public CppUserModBase {
public:
    PalworldEditorMod();
    ~PalworldEditorMod() override;

    /** @brief 在 UE4SS 完成 Unreal 初始化后注册游戏线程与世界切换回调。 */
    auto on_unreal_init() -> void override;

    /** @brief UE4SS UpdateThread 回调；刻意不访问 Unreal 或运行时请求队列。 */
    auto on_update() -> void override;

    /**
     * @brief 在 EngineTick 游戏线程消费全部 GUI 请求、执行反射操作并发布最新快照。
     * @warning 这是本类调用 Palworld 反射适配接口的唯一周期入口。
     */
    auto game_thread_tick(float deltaSeconds) -> void;

private:
    /** @brief 每个 EngineTick 最多尝试分类的被动技能数量。 */
    static constexpr std::size_t kPassiveMetadataBatchSize = 8;
    /** @brief 每个 EngineTick 被动技能分类反射的软时间预算。 */
    static constexpr auto kPassiveMetadataBudget = std::chrono::microseconds{500};

    /** @brief Unregisters one owned UE4SS callback if registration succeeded. */
    static auto unregister_callback(Hook::GlobalCallbackId& callbackId) -> void;

    /** @brief 推进爪钩、资源共享、远程终端及一次性诊断请求。 */
    auto process_runtime_services(float deltaSeconds) -> void;

    /** @brief 消费背包给予/修改/读取请求，并返回主背包安全门状态。 */
    [[nodiscard]] auto process_inventory_requests(float deltaSeconds) -> bool;

    /** @brief 消费目标选择、技能、属性和形态编辑请求。 */
    auto process_pal_edit_requests() -> void;

    /** @brief 推进物品目录、技能目录和被动技能分类初始化任务。 */
    auto process_initialization_tasks(bool worldContextReady) -> void;

    /** @brief 汇总可观察运行时值，并按脏标记向 GUI 发布技能快照。 */
    auto publish_runtime_state() -> void;

    /** @brief 消费复活与 UObject 发现等低频一次性请求。 */
    auto process_utility_requests() -> void;

    /** @brief 在游戏线程刷新技能目录，并为成功的被动目录建立增量分类任务。 */
    auto refresh_skill_catalog_on_game_thread() -> void;

    /** @brief 在当前 EngineTick 推进最多一个受数量和时间约束的分类批次。 */
    auto advance_passive_classification_on_game_thread() -> void;

    /** @brief 合并分类结果、应用失败回退并只发布一次最终目录快照。 */
    auto finish_passive_classification_on_game_thread() -> void;

    /** @brief 把爪钩游戏线程结果发布为 GUI 可安全复制的纯字符串。 */
    auto set_grapple_runtime_status(std::string status) -> void;

    /**
     * @brief 在 EngineTick 执行一次由纯值账本决定的爪钩应用或恢复工作。
     * @details 默认关闭、已应用和世界不可访问时立即返回，不扫描 UObject。
     */
    auto process_grapple_work(float deltaSeconds) -> void;

    /** @brief 遍历队伍槽位，复活所有处于死亡/濒死状态的帕鲁。 */
    auto revive_team_pals() -> void;

    /**
     * @brief 在关闭开关或切图前恢复全部活动爪钩覆盖。
     * @param[in] reason 用于错误消息的生命周期阶段。
     * @return 没有覆盖或恢复验证成功时返回 true。
     */
    [[nodiscard]] auto restore_grapple_overrides(std::string_view reason) -> bool;

    /** @brief Invalidates all work and write authorization before Unreal replaces the world. */
    auto begin_world_transition() -> void;

    /** @brief Re-enables reads after LoadMap without restoring Pal write authorization. */
    auto finish_world_transition() -> void;

    /** @brief 仅在可观察技能状态变化时把游戏线程快照发布给 GUI。 */
    auto publish_skill_snapshot_if_dirty() -> void;

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
        bool workSuitabilityWritesDisabled{}; /**< 本世界是否停用工作适应性写入。 */
        bool identityWritesDisabled{};        /**< 本世界是否因恢复失败而停用形态写入。 */
        pal_stats::PalStatSnapshot palStat;   /**< 最近一次从游戏重读的实际属性值。 */
        pal_identity::PalIdentitySnapshot palIdentity; /**< 最近一次形态字段重读快照。 */
    };

    /**
     * @brief 把整数限制到闭区间 `[lo, hi]`。
     * @return 限制后的整数。
     */
    static auto clamp(int v, int lo, int hi) -> int;

    /**
     * @brief 在技能目录中查找 Raw ID 对应的本地化标签。
     * @return 找到时返回 `本地化名称 [RawId]`，未找到时回退为原始 ID。
     */
    static auto find_skill_label(const std::vector<skill_editor::SkillOption>& options,
                                 std::string_view id) -> std::string;

    /**
     * @brief 在技能目录中查找 Raw ID 对应的目录项。
     * @return 找到时返回非空、非拥有的目录项指针；未找到时返回 `nullptr`。
     */
    [[nodiscard]] static auto find_skill_option(
        const std::vector<skill_editor::SkillOption>& options, std::string_view id)
        -> const skill_editor::SkillOption*;

    /**
     * @brief 清空与上一个技能目标相关的 GUI 临时编辑状态。
     * @param[in,out] self 非空、非拥有的当前 mod 实例指针。
     * @warning 只在 GUI 线程调用。
     */
    static void reset_skill_editor_ui(PalworldEditorMod* self);

    /**
     * @brief 渲染支持搜索、排除和单选的技能下拉框。
     * @retval true 本帧用户选择了不同的目录项。
     * @retval false 选择未发生变化。
     */
    static auto render_skill_picker(const char* id,
                                    const std::vector<skill_editor::SkillOption>& options,
                                    std::optional<skill_editor::ActiveSkillCategory> category,
                                    const std::unordered_set<std::string>& excludedIds,
                                    char* search, std::size_t searchSize,
                                    std::optional<skill_editor::SkillOption>& selected) -> bool;

    /** @brief 渲染物品 Raw ID 与数量输入，并提交给予物品请求。 */
    static void render_give_items(PalworldEditorMod* self);

    /** @brief 渲染可搜索的物品目录并把选中项的 Raw ID 填入给予输入框。 */
    static void render_item_browser(PalworldEditorMod* self);

    /** @brief 渲染主背包快照、当前选择以及槽位数量修改请求。 */
    static void render_inventory(PalworldEditorMod* self);

    /**
     * @brief 返回被动技能分类在界面上的固定中文名称。
     * @param[in] category 具体类别；空值表示“全部”。
     */
    [[nodiscard]] static auto passive_category_label(
        std::optional<skill_editor::PassiveSkillCategory> category) -> const char*;

    /**
     * @brief 返回被动技能分类在界面上的着色。
     * @return 普通/稀有/极品/传说/负面分别对应白/黄/蓝/紫/红的 ImGui 颜色。
     */
    [[nodiscard]] static auto passive_category_color(skill_editor::PassiveSkillCategory category)
        -> ImVec4;

    /**
     * @brief 渲染被动技能类别下拉框和分类进度/错误提示。
     * @warning 只在 GUI 线程调用。
     */
    static void render_passive_category_picker(PalworldEditorMod* self,
                                               const skill_editor::SkillCatalogSnapshot& catalog);

    /**
     * @brief 渲染按类别、排除集合和搜索词过滤的被动技能下拉框。
     * @retval true 本帧用户选择了不同的目录项。
     */
    static auto render_passive_skill_picker(PalworldEditorMod* self,
                                            const std::vector<skill_editor::SkillOption>& options,
                                            const std::unordered_set<std::string>& excludedIds)
        -> bool;

    /**
     * @brief 渲染被动技能列表及新增、替换、删除工作流。
     * @warning 只在 GUI 线程调用。
     */
    static void render_passive_skills(PalworldEditorMod* self, const SkillEditorSnapshot& snapshot,
                                      bool mutationsDisabled);

    /**
     * @brief 渲染三个 `EquipWaza` 主动技能槽及装备、替换、清空工作流。
     * @warning 只在 GUI 线程调用。
     */
    static void render_active_skills(PalworldEditorMod* self, const SkillEditorSnapshot& snapshot,
                                     bool mutationsDisabled);

    /** @brief 渲染同公会跨据点制作/建造材料共享开关与运行状态。 */
    static void render_base_resource_sharing(PalworldEditorMod* self);
    static void render_remote_palbox(PalworldEditorMod* self);

    /** @brief 渲染爪钩枪无冷却开关；切换时向游戏线程提交一次进程内请求。 */
    static void render_grapple_no_cooldown(PalworldEditorMod* self);

    /**
     * @brief 渲染 Alpha、Lucky 与觉醒三个彼此独立的显式应用开关。
     * @warning 只在 GUI 线程调用；控件不直接访问 Unreal 对象。
     */
    static void render_pal_identity(PalworldEditorMod* self, const SkillEditorSnapshot& snapshot,
                                    bool mutationsDisabled);

    /**
     * @brief 渲染持久化个体属性编辑区，点击应用后只提交相对快照发生变化的字段。
     * @warning 只在 GUI 线程调用。
     */
    static void render_pal_stats(PalworldEditorMod* self, const SkillEditorSnapshot& snapshot,
                                 bool mutationsDisabled, bool workSuitabilityMutationsDisabled);

    /**
     * @brief 渲染当前待出战帕鲁、技能目录状态和主动/被动技能编辑区域。
     * @warning 只在 GUI 线程调用。
     */
    static void render_pal_editor(PalworldEditorMod* self);

    /** @brief 渲染主窗口：应用主题 + 顶部 Tab 分页（物品/帕鲁/据点/诊断）。 */
    static void render_main_window(PalworldEditorMod* self);

    /** @brief 渲染诊断 Tab：发现对象按钮与预留诊断区。 */
    static void render_diagnostics(PalworldEditorMod* self);

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
    std::vector<pal_game::InvEntry> inv_cache_;
    /** @brief 最近一次物品扫描生成的本地化目录快照；由 inv_mutex_ 保护。 */
    item_catalog::ItemCatalogSnapshot item_db_cache_;

    /** @brief 请求游戏线程在下一次更新中刷新主背包快照。 */
    std::atomic<bool> want_read_{false};
    /** @brief 请求游戏线程在下一次更新中输出 UObject 诊断信息。 */
    std::atomic<bool> want_discover_{false};
    /** @brief GUI 线程提交、game_thread_tick 消费的一次性复活请求。 */
    std::atomic<bool> wantReviveTeam_{false};
    /** @brief 请求游戏线程在下一次更新中重新扫描物品目录。 */
    std::atomic<bool> want_scan_items_{false};
    /** @brief 主数据未就绪时按世界进行有界低频补全，不访问 Unreal。 */
    item_catalog::ItemCatalogScanScheduler itemCatalogScanScheduler_;
    /** @brief 请求首次 EngineTick 输出 UObject 诊断信息。 */
    std::atomic<bool> wantProbeObject_{false};

    /** @brief 游戏线程拥有的远程终端运行时；GUI 只读取其值快照。 */
    pal_remote_palbox::RemotePalboxRuntime remotePalboxRuntime_;
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
    /** @brief 在游戏线程执行 Alpha、Lucky 与觉醒反射事务的无所有权网关。 */
    pal_identity::PalIdentityGateway identityGateway_;
    /** @brief 属性恢复验证失败后阻止本世界继续写入；仅由游戏线程访问。 */
    bool statWritesDisabledForWorld_{};
    /** @brief 工作适应性恢复验证失败后只停用该属性域。 */
    bool workSuitabilityWritesDisabledForWorld_{};
    /** @brief GUI 生产、游戏线程消费且只保留最新状态的线程安全属性请求槽。 */
    pal_stats::PalStatEditRequestSlot statRequestSlot_;
    /** @brief 形态恢复验证失败后阻止本世界继续写入；仅由游戏线程访问。 */
    bool identityWritesDisabledForWorld_{};
    /** @brief GUI 生产、游戏线程消费且只保留最新状态的线程安全形态请求槽。 */
    pal_identity::PalIdentityEditRequestSlot identityRequestSlot_;
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
    /** @brief 以形态快照为基线、只生成三维差量请求的 GUI 草稿。 */
    pal_identity::PalIdentityEditDraft identityDraft_;
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
    /** @brief 主动技能目录的 Category 过滤；空值表示“全部”。 */
    std::optional<skill_editor::ActiveSkillCategory> activeCategoryFilter_;
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
