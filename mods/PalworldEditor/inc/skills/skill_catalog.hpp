/**
 * @file skill_catalog.hpp
 * @brief 提供与 Unreal 运行时无关的技能目录展示和筛选能力。
 * @details 本文件只负责技能目录展示和筛选，不执行任何游戏写入。
 */
#pragma once

#include <algorithm>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <skills/active_skill_definitions.hpp>

/**
 * @brief 定义技能目录展示、回退和筛选的纯值逻辑。
 */
namespace skill_editor {
/**
 * @brief 表示被动技能在编辑器中的品质分类。
 * @details “全部”属于界面过滤条件，不是被动技能自身的分类值。
 */
enum class PassiveSkillCategory {
    normal,    /**< 普通技能：`Rank` 为 0 到 2。 */
    rare,      /**< 稀有技能：`Rank` 为 3。 */
    premium,   /**< 极品技能：`Rank` 不小于 4。 */
    legendary, /**< 传说技能：`AddWorldTreePal` 为 `true`。 */
    negative,  /**< 负面技能：`Rank` 小于 0。 */
};

/**
 * @brief 保存分类被动技能所需的稳定运行时元数据。
 */
struct PassiveSkillMetadata {
    std::int32_t rank{};             /**< 数据表中的技能 Rank。 */
    bool addWorldTreePal{};          /**< 是否属于世界树被动技能池。 */
    PassiveSkillCategory category{}; /**< 根据运行时字段推导出的编辑器分类。 */

    auto operator<=>(const PassiveSkillMetadata&) const = default;
};

/**
 * @brief 根据 Palworld 运行时字段推导被动技能分类。
 * @param[in] rank 技能数据表中的 Rank。
 * @param[in] addWorldTreePal 技能是否属于世界树技能池。
 * @return 传说优先，其次依次判断负面、极品、稀有和普通。
 */
[[nodiscard]] constexpr auto classify_passive_skill(const std::int32_t rank,
                                                    const bool addWorldTreePal) noexcept
    -> PassiveSkillCategory {
    if (addWorldTreePal) {
        return PassiveSkillCategory::legendary;
    }
    if (rank < 0) {
        return PassiveSkillCategory::negative;
    }
    if (rank >= 4) {
        return PassiveSkillCategory::premium;
    }
    if (rank == 3) {
        return PassiveSkillCategory::rare;
    }
    return PassiveSkillCategory::normal;
}

/** @brief 主动技能的战斗类型，值与 Palworld EPalWazaCategory 对齐。 */
enum class ActiveSkillCategory : std::uint8_t {
    Melee = 0,    ///< 近战
    Shot = 1,     ///< 射击
    Support = 2,  ///< 辅助
};

/**
 * @brief 表示一个可供技能编辑界面展示的技能。
 */
struct SkillOption {
    std::string id;            /**< 技能的 Raw ID。 */
    std::string localizedName; /**< 当前游戏语言的展示名称；为空时界面回退到 `id`。 */
    std::optional<std::uint16_t> activeValue;          /**< 仅主动技能具有的 `EPalWazaID` 数值。 */
    std::optional<ActiveSkillCategory> activeCategory; /**< 仅主动技能具有的战斗类型。 */
    std::optional<PassiveSkillMetadata> passiveMetadata; /**< 仅被动技能具有的分类元数据。 */
};

/**
 * @brief 保存被动技能两级选择器的类别和技能选择。
 * @details 搜索文本由界面独立保存，因此切换类别只清空已选技能。
 */
struct PassiveSkillPickerState {
    std::optional<PassiveSkillCategory> category; /**< 空值表示“全部”。 */
    std::optional<SkillOption> selected;          /**< 当前待新增或替换的技能。 */

    /**
     * @brief 切换类别，并在实际变化时清空已选技能。
     * @param[in] nextCategory 新类别；空值表示“全部”。
     * @return 类别发生变化时返回 `true`。
     */
    [[nodiscard]] auto set_category(const std::optional<PassiveSkillCategory> nextCategory) noexcept
        -> bool {
        if (category == nextCategory) {
            return false;
        }
        category = nextCategory;
        selected.reset();
        return true;
    }

    /** @brief 清空技能选择但保留类别。 */
    void clear_selection() noexcept {
        selected.reset();
    }

    /** @brief 同时恢复“全部”类别并清空技能选择。 */
    void reset() noexcept {
        category.reset();
        selected.reset();
    }
};

/**
 * @brief 表示一次被动技能元数据读取的纯值结果。
 */
struct PassiveSkillMetadataReadResult {
    std::string id;                               /**< 已尝试读取的技能 Raw ID。 */
    std::optional<PassiveSkillMetadata> metadata; /**< 成功时的元数据；空值表示该 ID 未找到。 */
};

/**
 * @brief 表示一个受限反射批次的结果。
 */
struct PassiveSkillMetadataBatchResult {
    std::vector<PassiveSkillMetadataReadResult> entries; /**< 已实际完成的 ID 结果。 */
    std::string error;                                   /**< 阻止后续读取的结构性反射错误。 */
    std::chrono::microseconds elapsed{};                 /**< 本批次在游戏线程中的总耗时。 */
};

/**
 * @brief 表示被动技能分类任务可发布给界面的状态。
 */
struct PassiveSkillClassificationStatus {
    std::size_t completed{}; /**< 已完成或命中缓存的技能数量。 */
    std::size_t total{};     /**< 本次目录中的技能总数。 */
    std::string error;       /**< 结构性错误；为空表示未发生结构错误。 */
    bool ready{};            /**< 本轮所有 ID 均已尝试且没有结构错误。 */
};

/**
 * @brief 在刷新分类失败时决定具体类别是否仍可使用。
 * @param[in] status 本轮分类任务的最终状态。
 * @param[in] hadUsableSnapshot 刷新开始前是否已有完整可用的分类快照。
 * @return 保留原错误和进度；仅在已有旧分类时恢复具体类别可用状态。
 */
[[nodiscard]] inline auto with_passive_classification_fallback(
    PassiveSkillClassificationStatus status, const bool hadUsableSnapshot)
    -> PassiveSkillClassificationStatus {
    if (!status.error.empty() && hadUsableSnapshot) {
        status.ready = true;
    }
    return status;
}

/**
 * @brief 管理跨 EngineTick 的被动技能分类纯值状态。
 * @details 该类不接触 Unreal，也不拥有成功缓存；LoadMap 可安全取消任务而保留外部缓存。
 */
class PassiveSkillClassificationJob {
public:
    /**
     * @brief 为一个新目录建立分类任务。
     * @param[in] options 当前被动技能目录。
     * @param[in] successCache mod 生命周期内的成功元数据缓存。
     */
    void start(const std::span<const SkillOption> options,
               const std::unordered_map<std::string, PassiveSkillMetadata>& successCache) {
        cancel();
        started_ = true;
        total_ = options.size();
        for (const auto& option : options) {
            if (successCache.contains(option.id)) {
                ++completed_;
            } else {
                pending_.push_back(option.id);
            }
        }
        active_ = !pending_.empty();
    }

    /** @brief 取消当前任务且不修改调用方持有的成功缓存。 */
    void cancel() noexcept {
        pending_.clear();
        completed_ = 0;
        total_ = 0;
        error_.clear();
        active_ = false;
        started_ = false;
    }

    /**
     * @brief 以结构性错误终止当前任务。
     * @param[in] error 可供日志和界面展示的错误。
     */
    void fail(std::string error) {
        error_ = std::move(error);
        active_ = false;
    }

    /**
     * @brief 查看下一批待处理 ID，不提前修改队列。
     * @param[in] limit 本批允许返回的最大 ID 数。
     * @return 与目录顺序一致的 Raw ID 副本。
     */
    [[nodiscard]] auto next_batch(const std::size_t limit) const -> std::vector<std::string> {
        const auto count = std::min(limit, pending_.size());
        return {pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(count)};
    }

    /**
     * @brief 提交已实际执行的批次结果。
     * @param[in] entries 与待处理队首顺序一致的结果。
     * @param[in,out] successCache 仅写入成功取得的元数据。
     * @return 批次顺序合法时返回 `true`。
     */
    [[nodiscard]] auto complete_batch(
        const std::span<const PassiveSkillMetadataReadResult> entries,
        std::unordered_map<std::string, PassiveSkillMetadata>& successCache) -> bool {
        if (entries.size() > pending_.size()) {
            fail("passive classification batch order mismatch");
            return false;
        }

        auto pending = pending_.begin();
        for (const auto& entry : entries) {
            if (entry.id != *pending) {
                fail("passive classification batch order mismatch");
                return false;
            }
            ++pending;
        }

        for (const auto& entry : entries) {
            pending_.pop_front();
            ++completed_;
            if (entry.metadata.has_value()) {
                successCache.insert_or_assign(entry.id, *entry.metadata);
            }
        }
        active_ = !pending_.empty();
        return true;
    }

    /** @return 当前任务是否仍有待处理 ID。 */
    [[nodiscard]] auto active() const noexcept -> bool {
        return active_;
    }

    /** @return 当前进度、总数、结构错误和可用状态的纯值快照。 */
    [[nodiscard]] auto status() const -> PassiveSkillClassificationStatus {
        return {
            .completed = completed_,
            .total = total_,
            .error = error_,
            .ready = started_ && !active_ && error_.empty() && completed_ == total_,
        };
    }

private:
    std::deque<std::string> pending_; /**< 尚未尝试读取的 Raw ID。 */
    std::size_t completed_{};         /**< 已完成或命中缓存的数量。 */
    std::size_t total_{};             /**< 当前目录技能总数。 */
    std::string error_;               /**< 最近结构性错误。 */
    bool active_{};                   /**< 队列是否仍需在后续 EngineTick 推进。 */
    bool started_{};                  /**< 是否已经通过 `start()` 建立本轮任务。 */
};

/**
 * @brief 把成功缓存合并到当前被动技能目录。
 * @param[in,out] skills 当前被动技能目录；所有旧元数据会先按缓存结果更新。
 * @param[in] successCache mod 生命周期内的成功元数据缓存。
 */
inline void apply_passive_metadata(
    std::vector<SkillOption>& skills,
    const std::unordered_map<std::string, PassiveSkillMetadata>& successCache) {
    for (auto& skill : skills) {
        const auto found = successCache.find(skill.id);
        if (found != successCache.end()) {
            skill.passiveMetadata = found->second;
        } else {
            skill.passiveMetadata.reset();
        }
    }
}

/**
 * @brief Returns whether a stable active-skill Raw ID is clearly game-internal.
 */
[[nodiscard]] inline auto is_internal_active_skill_id(const std::string_view id) noexcept -> bool {
    return id.starts_with("Human_") || id.starts_with("Unique_") || id.contains("_GYM_") ||
           id.contains("Raid") || id.contains("Boss");
}

/**
 * @brief 从稳定的主动技能定义构建带当前语言名称的界面选项。
 * @tparam Localizer 接收 `ActiveSkillDefinition` 并返回 UTF-8 本地化名称的可调用对象。
 * @param[in] definitions 主动技能数值与 Raw ID 定义。
 * @param[in] localize 当前语言名称解析器；解析失败时返回空字符串。
 * @return 与输入顺序一致的主动技能选项。
 */
template <typename Localizer>
[[nodiscard]] auto make_active_skill_options(
    const std::span<const ActiveSkillDefinition> definitions, Localizer&& localize)
    -> std::vector<SkillOption> {
    std::vector<SkillOption> options;
    options.reserve(definitions.size());
    for (const auto& definition : definitions) {
        if (is_internal_active_skill_id(definition.id)) {
            continue;
        }

        std::string localizedName = localize(definition);
        if (localizedName.empty()) {
            continue;
        }
        options.push_back({
            .id = std::string(definition.id),
            .localizedName = std::move(localizedName),
            .activeValue = definition.value,
        });
    }
    return options;
}

/** @brief 保存一类技能目录的内容、最近错误和可用状态。 */
struct SkillCatalogSection {
    std::vector<SkillOption> skills; /**< 当前可展示和选择的技能目录。 */
    std::string error;               /**< 最近一次该类目录刷新产生的错误；为空表示没有错误。 */
    bool ready{};                    /**< 为 `true` 表示该类目录可供选择。 */
};

/**
 * @brief 保存相互独立的被动与主动技能目录刷新状态。
 */
struct SkillCatalogSnapshot {
    SkillCatalogSection passive;                            /**< 被动技能目录区段。 */
    SkillCatalogSection active;                             /**< 主动技能目录区段。 */
    PassiveSkillClassificationStatus passiveClassification; /**< 被动技能分类任务状态。 */
    bool runtimeReady{}; /**< 游戏技能与本地化运行时已完成一次完整加载。 */
};

/**
 * @brief 判断完整技能目录是否已验证为可安全编辑。
 * @param[in] catalog 当前技能目录快照。
 * @return 两类目录及游戏运行时都已就绪时返回 `true`。
 */
[[nodiscard]] inline auto catalog_is_ready_for_editing(const SkillCatalogSnapshot& catalog) noexcept
    -> bool {
    return catalog.runtimeReady && catalog.passive.ready && catalog.active.ready;
}

/**
 * @brief 对启动阶段的技能目录自动刷新进行节流。
 * @details 手动刷新会跳过时间节流，但仍必须先通过调用方提供的运行时安全检查；
 *          运行时就绪后停止自动刷新。
 */
class SkillCatalogRefreshScheduler {
public:
    using clock = std::chrono::steady_clock;
    using time_point = clock::time_point;

    /**
     * @brief 建立指定自动重试间隔的调度器。
     * @param[in] retryInterval 两次自动刷新之间的最短时间。
     */
    explicit SkillCatalogRefreshScheduler(const clock::duration retryInterval)
        : retryInterval_(retryInterval) {}

    /**
     * @brief 判断当前更新是否应执行完整目录刷新。
     * @param[in] manual 是否由用户手动请求刷新。
     * @param[in] ready 技能运行时是否已完成一次完整加载。
     * @param[in] now 当前稳定时钟时间点。
     * @param[in] runtimeReady 仅在刷新到期时调用；返回是否可安全查询游戏技能运行时。
     * @return 本次更新应执行刷新时返回 `true`。
     */
    template <typename RuntimeReady>
    [[nodiscard]] auto should_refresh(const bool manual, const bool ready, const time_point now,
                                      RuntimeReady&& runtimeReady) -> bool {
        if (!manual && (ready || (hasAttempted_ && now < nextAutomaticRefresh_))) {
            return false;
        }

        hasAttempted_ = true;
        nextAutomaticRefresh_ = now + retryInterval_;
        return std::forward<RuntimeReady>(runtimeReady)();
    }

private:
    clock::duration retryInterval_;   /**< 自动刷新之间的最短间隔。 */
    time_point nextAutomaticRefresh_; /**< 下一次允许自动刷新的时间点。 */
    bool hasAttempted_{};             /**< 是否已经执行过首次自动刷新。 */
};

/**
 * @brief 在单个目录区段刷新失败时保留上一份可用内容。
 * @param[in] previous 上一份区段快照。
 * @param[in] refreshed 最新刷新得到的区段快照。
 * @return 新区段可用或旧区段不可用时返回新区段；否则保留旧内容并传播最新错误。
 */
[[nodiscard]] inline auto with_section_fallback(const SkillCatalogSection& previous,
                                                const SkillCatalogSection& refreshed)
    -> SkillCatalogSection {
    if (refreshed.ready || !previous.ready) {
        return refreshed;
    }

    auto fallback = previous;
    fallback.error = refreshed.error;
    return fallback;
}

/**
 * @brief 分别合并被动与主动技能目录的刷新结果。
 * @param[in] previous 上一份完整目录快照。
 * @param[in] refreshed 最新完整目录刷新结果。
 * @return 两个区段分别应用回退规则后的目录快照。
 */
[[nodiscard]] inline auto with_catalog_fallback(const SkillCatalogSnapshot& previous,
                                                const SkillCatalogSnapshot& refreshed)
    -> SkillCatalogSnapshot {
    return {
        .passive = with_section_fallback(previous.passive, refreshed.passive),
        .active = with_section_fallback(previous.active, refreshed.active),
        .passiveClassification = refreshed.passive.ready ? refreshed.passiveClassification
                                                         : previous.passiveClassification,
        .runtimeReady = previous.runtimeReady || refreshed.runtimeReady,
    };
}

/**
 * @brief 将 ASCII 大写字母转换为小写字母。
 * @param[in] value 待规范化的文本视图；函数不保存该视图。
 * @return 仅改变 ASCII 大写字母后的副本。
 */
[[nodiscard]] inline auto ascii_lower(const std::string_view value) -> std::string {
    std::string result(value);
    for (auto& character : result) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return result;
}

/**
 * @brief 生成技能的界面展示标签。
 * @param[in] option 要生成标签的技能。
 * @return 本地化名称非空时为“名称 [Raw ID]”，否则为 Raw ID。
 */
[[nodiscard]] inline auto skill_label(const SkillOption& option) -> std::string {
    if (option.localizedName.empty()) {
        return option.id;
    }
    return option.localizedName + " [" + option.id + "]";
}

/**
 * @brief 判断技能是否匹配不区分 ASCII 大小写的搜索词。
 * @param[in] option 要检查的技能。
 * @param[in] query 搜索文本；为空时匹配所有技能。
 * @return 搜索词为空，或其出现在 Raw ID 或本地化名称中时为 `true`。
 */
[[nodiscard]] inline auto matches_skill(const SkillOption& option, const std::string_view query)
    -> bool {
    if (query.empty()) {
        return true;
    }

    const auto normalizedQuery = ascii_lower(query);
    return ascii_lower(option.id).contains(normalizedQuery) ||
           ascii_lower(option.localizedName).contains(normalizedQuery);
}

/**
 * @brief 按 Raw ID 去重技能列表。
 * @param[in] options 待去重的技能值列表。
 * @return 忽略空 Raw ID，并保留每个 Raw ID 首次出现的技能副本。
 */
[[nodiscard]] inline auto deduplicate_skills(std::vector<SkillOption> options)
    -> std::vector<SkillOption> {
    std::unordered_set<std::string> seen;
    std::vector<SkillOption> result;
    result.reserve(options.size());
    for (auto& option : options) {
        if (!option.id.empty() && seen.insert(option.id).second) {
            result.push_back(std::move(option));
        }
    }
    return result;
}

/**
 * @brief 筛选未被排除且匹配搜索词的技能。
 * @param[in] options 待筛选的技能视图。
 * @param[in] query 搜索文本；为空时不限制匹配。
 * @param[in] excludedIds 需要排除的技能 Raw ID 集合。
 * @return 同时应用搜索和排除集合后得到的技能值拷贝。
 */
[[nodiscard]] inline auto filter_skills(const std::span<const SkillOption> options,
                                        const std::string_view query,
                                        const std::unordered_set<std::string>& excludedIds)
    -> std::vector<SkillOption> {
    std::vector<SkillOption> result;
    for (const auto& option : options) {
        if (!excludedIds.contains(option.id) && matches_skill(option, query)) {
            result.push_back(option);
        }
    }
    return result;
}

/**
 * @brief 按类别、排除集合和搜索词筛选被动技能。
 * @param[in] options 待筛选的被动技能目录。
 * @param[in] category 具体类别；空值表示“全部”。
 * @param[in] query 中文名或 Raw ID 搜索文本。
 * @param[in] excludedIds 已装备且不应再次选择的被动技能 Raw ID。
 * @return 按“类别、排除、搜索”顺序过滤后的技能值副本。
 */
[[nodiscard]] inline auto filter_passive_skills(const std::span<const SkillOption> options,
                                                const std::optional<PassiveSkillCategory> category,
                                                const std::string_view query,
                                                const std::unordered_set<std::string>& excludedIds)
    -> std::vector<SkillOption> {
    std::vector<SkillOption> result;
    for (const auto& option : options) {
        if (category.has_value() && (!option.passiveMetadata.has_value() ||
                                     option.passiveMetadata->category != *category)) {
            continue;
        }
        if (excludedIds.contains(option.id) || !matches_skill(option, query)) {
            continue;
        }
        result.push_back(option);
    }
    return result;
}

/**
 * @brief 按类别、排除集合和搜索词生成不复制技能值的被动技能视图。
 * @param[in] options 待筛选的被动技能目录；返回指针的有效期不超过该目录。
 * @param[in] category 具体类别；空值表示“全部”。
 * @param[in] query 中文名或 Raw ID 搜索文本。
 * @param[in] excludedIds 已装备且不应再次选择的被动技能 Raw ID。
 * @return 与原目录顺序一致、指向原目录元素的非拥有指针。
 */
[[nodiscard]] inline auto filter_passive_skill_views(
    const std::span<const SkillOption> options, const std::optional<PassiveSkillCategory> category,
    const std::string_view query, const std::unordered_set<std::string>& excludedIds)
    -> std::vector<const SkillOption*> {
    std::vector<const SkillOption*> result;
    result.reserve(options.size());
    for (const auto& option : options) {
        if (category.has_value() && (!option.passiveMetadata.has_value() ||
                                     option.passiveMetadata->category != *category)) {
            continue;
        }
        if (excludedIds.contains(option.id) || !matches_skill(option, query)) {
            continue;
        }
        result.push_back(&option);
    }
    return result;
}
}  // namespace skill_editor
