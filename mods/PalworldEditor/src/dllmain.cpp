/**
 * @file dllmain.cpp
 * @brief 实现 PalworldEditor mod 生命周期、ImGui 界面、跨线程请求交接和 DLL 导出入口。
 * @details ImGui 回调运行在 GUI 线程，只读取互斥量保护的快照并提交请求；on_update()
 *          运行在游戏线程，是执行 Unreal 反射操作的唯一入口。结果通过互斥量保护的缓存和
 *          技能快照返回 GUI。构建使用 `cmake --preset ninja-msvc-x64`，部署使用
 *          `cmake --build --preset ninja-msvc-x64 --target deploy`。
 */

#include <atomic>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include <DynamicOutput/DynamicOutput.hpp>
#include <GUI/GUITab.hpp>
#include <Mod/CppUserModBase.hpp>
#include <UE4SSProgram.hpp>                           // UE4SS_ENABLE_IMGUI
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>  // FProperty (for viewed-Pal name read)
#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <game/pal_game.hpp>
#include <imgui.h>
#include <items/item_catalog.hpp>
#include <skills/pal_skills.hpp>
#include <support/text_encoding.hpp>

using namespace RC;
using namespace RC::Unreal;
using pal_game::InvEntry;
using pal_game::PalEntry;

/**
 * @brief PalworldEditor 的 UE4SS mod 实例与全部运行时状态容器。
 * @details 类本身拥有 GUI 状态、请求队列和值类型缓存，但不拥有任何 Unreal UObject。
 *          GUI 线程不得直接调用反射接口；游戏线程通过 on_update() 消费请求并发布快照。
 */
class PalworldEditorMod final : public CppUserModBase {
public:
    /**
     * @brief 初始化 mod 元数据并注册 `PalworldEditor` ImGui 页签。
     * @details 构造阶段不访问 Unreal UObject；页签回调只调用本类的静态渲染辅助函数。
     */
    PalworldEditorMod() : CppUserModBase() {
        ModName = STR("PalworldEditor");
        ModVersion = STR("1.4.0");
        ModDescription = STR("In-game item and active/passive Pal skill editor for Palworld 1.0");
        ModAuthors = STR("with-fair-wind");

        Output::send<LogLevel::Verbose>(STR("PalworldEditor loaded (v1.4.0)\n"));

        register_tab(STR("PalworldEditor"), [](CppUserModBase* mod) {
            UE4SS_ENABLE_IMGUI()
            auto* self = static_cast<PalworldEditorMod*>(mod);
            ImGui::TextUnformatted("A floating 'PalworldEditor' window should be visible ->");
            if (ImGui::Begin("PalworldEditor v1.4.0", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                render_give_items(self);
                ImGui::Separator();
                render_item_browser(self);
                ImGui::Separator();
                render_inventory(self);
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

    /**
     * @brief 销毁 mod 持有的 C++ 状态。
     * @details 本类不拥有 Unreal 对象或手动资源，因此使用默认析构行为。
     */
    ~PalworldEditorMod() override = default;

    /**
     * @brief 在 UE4SS 完成 Unreal 初始化后建立对象探测和查看帕鲁追踪。
     * @details 注册 `GetPassiveSkillList` 的 `ProcessEvent` 前置回调；外部游戏调用该函数时，
     *          回调把 Context 作为非拥有目标句柄记录到 lastViewedPal_。最后请求首次物品扫描。
     * @warning 由 UE4SS 在游戏线程调用。
     */
    auto on_unreal_init() -> void override {
        if (const auto object = UObjectGlobals::StaticFindObject<UObject*>(
                nullptr, nullptr, STR("/Script/CoreUObject.Object"))) {
            Output::send<LogLevel::Verbose>(STR("Object Name: {}\n"), object->GetFullName());
        }

        // Track which Pal the player is viewing (GetPassiveSkillList hook).
        fnGetPSL_ = UObjectGlobals::StaticFindObject<UFunction*>(
            nullptr, nullptr,
            STR("/Script/Pal.PalIndividualCharacterParameter:GetPassiveSkillList"));
        if (fnGetPSL_ != nullptr) {
            Hook::RegisterProcessEventPreCallback(
                [this](auto& /*info*/, UObject* Context, UFunction* Function, void* /*Params*/) {
                    if (Function == fnGetPSL_ && Context != nullptr &&
                        !suppressViewTracking_.load()) {
                        lastViewedPal_.store(reinterpret_cast<uintptr_t>(Context));
                    }
                },
                Hook::FCallbackOptions{.OwnerModName = STR("PalworldEditor"),
                                       .HookName = STR("PalViewTracker")});
            Output::send<LogLevel::Verbose>(
                STR("PalworldEditor: PalViewTracker hook registered\n"));
        }

        want_scan_items_.store(true);
    }

    /**
     * @brief 在游戏线程消费全部 GUI 请求、执行反射操作并发布最新快照。
     * @details 单次更新依次处理查看目标名称、给予物品、背包数量修改、背包/物品/帕鲁扫描、
     *          技能编辑 FIFO 队列、技能目录/状态刷新和诊断扫描。共享结果在相应互斥量保护下写回。
     * @warning 这是本类调用 Palworld 反射适配接口的唯一周期入口，由 UE4SS 在游戏线程调用。
     */
    auto on_update() -> void override {
        // Cache the viewed Pal's species name when pointer changes.
        const uintptr_t viewed = lastViewedPal_.load();
        if (viewed != lastCachedPal_) {
            lastCachedPal_ = viewed;
            std::string name;
            if (skillGateway_.is_valid(viewed)) {
                UObject* pal = reinterpret_cast<UObject*>(viewed);
                if (FProperty* spProp = pal->GetPropertyByNameInChain(STR("SaveParameter"))) {
                    if (FName* charId = spProp->ContainerPtrToValuePtr<FName>(pal)) {
                        const std::wstring w = charId->ToString();
                        name = text_encoding::to_utf8(w);
                    }
                }
            }
            {
                const std::lock_guard lock(inv_mutex_);
                viewedPalName_ = std::move(name);
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

        // Scan pals
        if (want_scan_pals_.exchange(false)) {
            auto fresh = pal_game::scan_pals();
            const std::lock_guard lock(inv_mutex_);
            if (pal_selected_ >= static_cast<int>(fresh.size())) {
                pal_selected_ = -1;
            }
            pal_cache_ = std::move(fresh);
        }

        std::optional<skill_editor::SkillEditResult> editResult;
        if (auto request = skillQueue_.try_pop()) {
            {
                const std::lock_guard lock(skillSnapshotMutex_);
                skillSnapshot_.pending = true;
            }
            ViewTrackingGuard guard(suppressViewTracking_);
            editResult = skill_editor::execute_skill_edit(skillGateway_, *request);
        }

        const auto resolved = resolve_skill_target();
        const bool targetChanged = resolved.target != lastSkillTarget_;
        lastSkillTarget_ = resolved.target;

        std::optional<skill_editor::SkillCatalogSnapshot> refreshedCatalog;
        if (resolved.target != 0 && wantRefreshSkillCatalog_.exchange(false)) {
            skill_editor::SkillCatalogSnapshot previous;
            {
                const std::lock_guard lock(skillSnapshotMutex_);
                previous = skillSnapshot_.catalog;
            }
            ViewTrackingGuard guard(suppressViewTracking_);
            refreshedCatalog = skill_editor::with_catalog_fallback(
                previous, skillGateway_.load_catalog());
        }

        std::optional<skill_editor::SkillState> refreshedState;
        if (resolved.target != 0 && (targetChanged || editResult.has_value())) {
            ViewTrackingGuard guard(suppressViewTracking_);
            refreshedState = skillGateway_.read_state(resolved.target);
        }

        {
            const std::lock_guard lock(skillSnapshotMutex_);
            skillSnapshot_.target = resolved.target;
            skillSnapshot_.source = resolved.source;
            skillSnapshot_.palName = resolved.name;
            skillSnapshot_.pending = skillQueue_.size() != 0;
            if (refreshedCatalog.has_value()) {
                skillSnapshot_.catalog = std::move(*refreshedCatalog);
            }
            if (refreshedState.has_value()) {
                skillSnapshot_.state = std::move(*refreshedState);
            } else if (resolved.target == 0) {
                skillSnapshot_.state = {};
            }
            if (editResult.has_value()) {
                skillSnapshot_.lastResult = editResult->message;
            }
        }

        // Discover
        if (want_discover_.exchange(false)) {
            pal_game::discover_objects();
        }
    }

private:
    /** @brief 描述当前技能编辑目标的解析来源。 */
    enum class SkillTargetSource {
        none,     /**< 当前没有有效技能目标。 */
        viewed,   /**< 目标来自 `GetPassiveSkillList` Hook 捕获的当前查看帕鲁。 */
        selected, /**< 目标来自扫描列表中的手动选择。 */
    };

    /** @brief 一次技能目标解析得到的非拥有目标句柄、来源和展示名称。 */
    struct ResolvedSkillTarget {
        skill_editor::SkillTarget target{}; /**< 非拥有帕鲁句柄；为 0 时表示没有有效目标。 */
        SkillTargetSource source{SkillTargetSource::none}; /**< 目标解析来源。 */
        std::string name; /**< 用于 GUI 展示的帕鲁名称；尚未读取时可为空。 */
    };

    /**
     * @brief 游戏线程发布给 GUI 的完整技能编辑快照。
     * @details 此结构按值复制，避免 GUI 在持锁期间执行复杂渲染逻辑。
     */
    struct SkillEditorSnapshot {
        skill_editor::SkillTarget target{};                /**< 当前非拥有技能目标句柄。 */
        SkillTargetSource source{SkillTargetSource::none}; /**< 当前目标的解析来源。 */
        std::string palName;                               /**< 当前目标的 GUI 展示名称。 */
        skill_editor::SkillState state;             /**< 最近一次从游戏重读的实际技能状态。 */
        skill_editor::SkillCatalogSnapshot catalog; /**< 最近一份可用的运行时技能目录。 */
        std::string lastResult;                     /**< 最近一次技能编辑结果的面向用户消息。 */
        bool pending{}; /**< 技能请求队列中是否仍有待游戏线程处理的请求。 */
    };

    /**
     * @brief 在本 mod 主动调用技能读取反射时临时抑制查看目标追踪。
     * @details 构造时把共享原子标志设为 `true`，析构时恢复为 `false`，防止本 mod 的
     *          `GetPassiveSkillList` 调用被 Hook 误判为玩家正在查看新的帕鲁。
     */
    class ViewTrackingGuard {
    public:
        /**
         * @brief 开始抑制查看目标追踪。
         * @param[in,out] flag 生命周期长于本守卫的共享原子标志；守卫不拥有该对象。
         */
        explicit ViewTrackingGuard(std::atomic<bool>& flag) : flag_(flag) {
            flag_.store(true);
        }

        /** @brief 结束抑制并恢复查看目标追踪。 */
        ~ViewTrackingGuard() {
            flag_.store(false);
        }

        /** @brief 禁止复制，避免多个守卫重复恢复同一标志。 */
        ViewTrackingGuard(const ViewTrackingGuard&) = delete;

        /**
         * @brief 禁止复制赋值，保证恢复动作只有一个责任对象。
         * @return 不会返回；该操作在编译期被删除。
         */
        auto operator=(const ViewTrackingGuard&) -> ViewTrackingGuard& = delete;

    private:
        /** @brief 被临时置位的非拥有原子标志引用。 */
        std::atomic<bool>& flag_;
    };

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
     * @brief 解析当前技能编辑目标，优先使用正在查看的帕鲁。
     * @return 已解析目标；查看目标无效时退回扫描列表手动选择，两者均无效时返回空结构。
     * @details 读取手动选择和名称时持有 inv_mutex_，返回的目标句柄仍是非拥有值。
     * @warning 由游戏线程调用，并在返回前通过 skillGateway_ 重新校验目标。
     */
    auto resolve_skill_target() -> ResolvedSkillTarget {
        const auto viewed = lastViewedPal_.load();
        if (skillGateway_.is_valid(viewed)) {
            std::string name;
            {
                const std::lock_guard lock(inv_mutex_);
                name = viewedPalName_;
            }
            return {.target = viewed, .source = SkillTargetSource::viewed, .name = std::move(name)};
        }

        UObject* selected = nullptr;
        std::string name;
        {
            const std::lock_guard lock(inv_mutex_);
            if (pal_selected_ >= 0 && pal_selected_ < static_cast<int>(pal_cache_.size())) {
                selected = pal_cache_[pal_selected_].ptr;
                name = pal_cache_[pal_selected_].name;
            }
        }
        const auto selectedTarget = reinterpret_cast<skill_editor::SkillTarget>(selected);
        if (skillGateway_.is_valid(selectedTarget)) {
            return {.target = selectedTarget,
                    .source = SkillTargetSource::selected,
                    .name = std::move(name)};
        }
        return {};
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
     * @param[in] pending 是否存在尚未处理完成的技能编辑请求。
     * @details 删除请求立即进入 FIFO；新增和替换先进入选择状态，确认后使用 Raw ID 提交。
     * @warning 只在 GUI 线程调用。
     */
    static void render_passive_skills(PalworldEditorMod* self, const SkillEditorSnapshot& snapshot,
                                      const bool pending) {
        ImGui::Text("被动技能 (%d/4)", static_cast<int>(snapshot.state.passiveIds.size()));
        std::unordered_set<std::string> excluded(snapshot.state.passiveIds.begin(),
                                                 snapshot.state.passiveIds.end());

        ImGui::BeginDisabled(pending);
        for (std::size_t index = 0; index < snapshot.state.passiveIds.size(); ++index) {
            const auto& id = snapshot.state.passiveIds[index];
            const auto label = find_skill_label(snapshot.catalog.passiveSkills, id);
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
                self->skillQueue_.push({.target = snapshot.target,
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
        ImGui::BeginDisabled(pending || !snapshot.catalog.ready);
        render_skill_picker("##passive-picker", snapshot.catalog.passiveSkills, excluded,
                            self->passiveSearch_, sizeof(self->passiveSearch_),
                            self->passiveChoice_);
        const bool canConfirm =
            self->passiveChoice_.has_value() &&
            (!replacing ||
             self->passiveEditIndex_ < static_cast<int>(snapshot.state.passiveIds.size()));
        ImGui::BeginDisabled(!canConfirm);
        if (ImGui::Button("确认被动技能修改")) {
            skill_editor::SkillEditRequest request{
                .target = snapshot.target,
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
     * @param[in] pending 是否存在尚未处理完成的技能编辑请求。
     * @details 新技能只能追加到第一个尾部空槽；提交时同时携带 Raw ID 和 `EPalWazaID` 数值。
     * @warning 只在 GUI 线程调用。
     */
    static void render_active_skills(PalworldEditorMod* self, const SkillEditorSnapshot& snapshot,
                                     const bool pending) {
        ImGui::TextUnformatted("主动技能（EquipWaza）");
        std::unordered_set<std::string> excluded;
        for (const auto& skill : snapshot.state.activeSkills) {
            excluded.insert(skill.id);
        }

        ImGui::BeginDisabled(pending);
        for (std::size_t slot = 0; slot < 3; ++slot) {
            if (slot < snapshot.state.activeSkills.size()) {
                const auto& skill = snapshot.state.activeSkills[slot];
                const auto label = find_skill_label(snapshot.catalog.activeSkills, skill.id);
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
                    self->skillQueue_.push({.target = snapshot.target,
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
        ImGui::BeginDisabled(pending || !snapshot.catalog.ready);
        render_skill_picker("##active-picker", snapshot.catalog.activeSkills, excluded,
                            self->activeSearch_, sizeof(self->activeSearch_), self->activeChoice_);
        const bool canConfirm =
            self->activeChoice_.has_value() && self->activeChoice_->activeValue.has_value();
        ImGui::BeginDisabled(!canConfirm);
        if (ImGui::Button("确认主动技能修改")) {
            self->skillQueue_.push(
                {.target = snapshot.target,
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

    /**
     * @brief 渲染帕鲁目标选择、技能目录状态和主动/被动技能编辑区域。
     * @param[in,out] self 非空、非拥有的当前 mod 实例指针。
     * @details 优先展示 Hook 自动发现的查看目标；没有有效查看目标时允许从 pal_cache_
     *          手动选择。渲染前复制 skillSnapshot_，目标变化时重置临时 UI 状态。
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
        if (self->skillUiTarget_ != snapshot.target) {
            self->skillUiTarget_ = snapshot.target;
            reset_skill_editor_ui(self);
        }

        if (snapshot.target != 0) {
            const char* source =
                snapshot.source == SkillTargetSource::viewed ? "当前查看" : "手动选择";
            ImGui::TextColored(ImVec4(0.4F, 1.0F, 0.4F, 1.0F), "目标：%s（%s）",
                               snapshot.palName.empty() ? "(读取中...)" : snapshot.palName.c_str(),
                               source);
        } else {
            ImGui::TextDisabled("请在游戏中查看一只帕鲁，或从下方扫描列表中手动选择。");
        }
        const bool pending = snapshot.pending || self->skillQueue_.size() != 0;
        ImGui::SameLine();
        ImGui::BeginDisabled(pending || snapshot.target == 0);
        if (ImGui::Button("刷新技能列表")) {
            self->wantRefreshSkillCatalog_.store(true);
        }
        ImGui::EndDisabled();
        if (pending) {
            ImGui::TextColored(ImVec4(1.0F, 0.8F, 0.2F, 1.0F), "技能修改处理中...");
        }
        if (!snapshot.lastResult.empty()) {
            ImGui::TextWrapped("结果：%s", snapshot.lastResult.c_str());
        }
        if (!snapshot.catalog.error.empty()) {
            ImGui::TextColored(ImVec4(1.0F, 0.45F, 0.35F, 1.0F), "技能目录：%s",
                               snapshot.catalog.error.c_str());
        }
        ImGui::Separator();

        // Pal list (manual selection fallback)
        if (ImGui::Button("Scan Pals")) {
            self->want_scan_pals_.store(true);
        }
        ImGui::SameLine();
        {
            const std::lock_guard lock(self->inv_mutex_);
            ImGui::TextDisabled("(%d pals)", static_cast<int>(self->pal_cache_.size()));
        }
        ImGui::BeginChild("pallist", ImVec2(380, 140), true);
        {
            const std::lock_guard lock(self->inv_mutex_);
            for (int i = 0; i < static_cast<int>(self->pal_cache_.size()); ++i) {
                const std::string palLabel =
                    self->pal_cache_[i].name + " ##pal" + std::to_string(i);
                if (ImGui::Selectable(palLabel.c_str(), self->pal_selected_ == i)) {
                    self->pal_selected_ = i;
                }
            }
        }
        ImGui::EndChild();

        if (snapshot.target == 0) {
            return;
        }

        ImGui::Separator();
        render_passive_skills(self, snapshot, pending);
        ImGui::Separator();
        render_active_skills(self, snapshot, pending);
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
     * @details GUI 线程写入参数，游戏线程在 on_update() 中复制参数。
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
     * @brief 保护背包、物品目录、帕鲁扫描缓存及查看目标名称。
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

    /** @brief GUI 手动选择的 pal_cache_ 索引；`-1` 表示未选择，由 inv_mutex_ 保护。 */
    int pal_selected_ = -1;
    /** @brief 最近一次游戏线程扫描的已加载帕鲁快照；由 inv_mutex_ 保护。 */
    std::vector<PalEntry> pal_cache_;
    /** @brief 请求游戏线程在下一次更新中重新扫描帕鲁对象。 */
    std::atomic<bool> want_scan_pals_{false};

    /**
     * @brief Hook 最近捕获的查看帕鲁非拥有句柄。
     * @details ProcessEvent 回调写入，游戏线程和 GUI 目标解析流程读取；0 表示未知。
     */
    std::atomic<uintptr_t> lastViewedPal_{0};
    /** @brief 临时禁止本 mod 自身反射读取更新 lastViewedPal_ 的原子标志。 */
    std::atomic<bool> suppressViewTracking_{false};
    /**
     * @brief `GetPassiveSkillList` 的非拥有 UFunction 观察指针。
     * @details 在 on_unreal_init() 中解析，供 ProcessEvent Hook 比较函数身份。
     */
    UFunction* fnGetPSL_{};
    /** @brief lastViewedPal_ 对应的 Raw 物种名称；由 inv_mutex_ 保护。 */
    std::string viewedPalName_;
    /** @brief 游戏线程上一次已生成名称缓存的查看目标句柄。 */
    uintptr_t lastCachedPal_{0};

    /** @brief 在游戏线程执行 Palworld 技能反射读写的无 UObject 所有权网关。 */
    pal_skills::PalSkillGateway skillGateway_;
    /** @brief GUI 生产、游戏线程 FIFO 消费的线程安全技能编辑请求队列。 */
    skill_editor::SkillEditQueue skillQueue_;
    /** @brief 保护游戏线程发布、GUI 线程复制的 skillSnapshot_。 */
    std::mutex skillSnapshotMutex_;
    /** @brief 最近一次发布给 GUI 的完整技能编辑快照；由 skillSnapshotMutex_ 保护。 */
    SkillEditorSnapshot skillSnapshot_;
    /** @brief 请求游戏线程重新加载运行时技能目录；初始值触发首次加载。 */
    std::atomic<bool> wantRefreshSkillCatalog_{true};
    /** @brief 游戏线程上一帧解析到的技能目标，用于检测目标变化。 */
    skill_editor::SkillTarget lastSkillTarget_{};

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
    /** @brief GUI 上一次渲染的技能目标；变化时用于触发临时编辑状态重置。 */
    skill_editor::SkillTarget skillUiTarget_{};
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
