/**
 * @file skill_catalog.hpp
 * @brief 提供与 Unreal 运行时无关的技能目录展示和筛选能力。
 * @details 本文件只负责技能目录展示和筛选，不执行任何游戏写入。
 */
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <skills/active_skill_definitions.hpp>

/**
 * @brief 定义技能目录展示、回退和筛选的纯值逻辑。
 */
namespace skill_editor {
/**
 * @brief 表示一个可供技能编辑界面展示的技能。
 */
struct SkillOption {
    std::string id;            /**< 技能的 Raw ID。 */
    std::string localizedName; /**< 当前游戏语言的展示名称；为空时界面回退到 `id`。 */
    std::optional<std::uint16_t> activeValue; /**< 仅主动技能具有的 `EPalWazaID` 数值。 */
};

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
        options.push_back({
            .id = std::string(definition.id),
            .localizedName = localize(definition),
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
    SkillCatalogSection passive; /**< 被动技能目录区段。 */
    SkillCatalogSection active;  /**< 主动技能目录区段。 */
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
}  // namespace skill_editor
