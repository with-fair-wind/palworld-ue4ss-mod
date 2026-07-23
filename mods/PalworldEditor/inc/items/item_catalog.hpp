/**
 * @file item_catalog.hpp
 * @brief 提供与 Unreal 运行时无关的物品目录整理、标签生成和搜索能力。
 * @details 本文件只处理 UTF-8 字符串和值类型，不持有任何游戏对象。
 */
#pragma once

#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

/**
 * @brief 定义物品目录的纯值模型、标签和搜索逻辑。
 */
namespace item_catalog {
/**
 * @brief 表示一个可供界面选择的物品。
 */
struct ItemOption {
    std::string id;            /**< 写入 Palworld 接口的物品 Raw ID。 */
    std::string localizedName; /**< 当前游戏语言的展示名称；为空时界面回退到 `id`。 */
};

/**
 * @brief 保存经过整理的物品目录及其显示标签索引。
 */
struct ItemCatalogSnapshot {
    std::vector<ItemOption> items; /**< 去重并按最终标签稳定排序的物品。 */
    std::unordered_map<std::string, std::string>
        labelsById; /**< 以 Raw ID 映射到展示标签的索引。 */
};

/**
 * @brief 将 ASCII 大写字母转换为小写字母。
 * @param[in] value 待规范化的文本视图；函数不保存该视图。
 * @return 仅改变 ASCII 大写字母后的副本。
 */
[[nodiscard]] inline auto ascii_lower(std::string_view value) -> std::string {
    std::string lowered(value);
    for (auto& character : lowered) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return lowered;
}

/**
 * @brief 生成物品的界面展示标签。
 * @param[in] item 要生成标签的物品。
 * @return 本地化名称非空时为“名称 [Raw ID]”，否则为 Raw ID。
 */
[[nodiscard]] inline auto item_label(const ItemOption& item) -> std::string {
    if (item.localizedName.empty()) {
        return item.id;
    }
    return item.localizedName + " [" + item.id + ']';
}

/**
 * @brief 从目录索引查找物品展示标签。
 * @param[in] catalog 包含标签索引的物品目录。
 * @param[in] id 要查询的物品 Raw ID；函数不保存该视图。
 * @return 已索引的展示标签；未找到时返回 Raw ID 的副本。
 */
[[nodiscard]] inline auto item_label(const ItemCatalogSnapshot& catalog, const std::string_view id)
    -> std::string {
    const auto found = catalog.labelsById.find(std::string(id));
    return found == catalog.labelsById.end() ? std::string(id) : found->second;
}

/**
 * @brief 判断物品是否匹配不区分 ASCII 大小写的搜索词。
 * @param[in] item 要检查的物品。
 * @param[in] query 搜索文本；为空时匹配所有物品。
 * @return 搜索词为空，或其出现在 Raw ID 或本地化名称中时为 `true`。
 */
[[nodiscard]] inline auto matches_item(const ItemOption& item, const std::string_view query)
    -> bool {
    const auto loweredQuery = ascii_lower(query);
    return loweredQuery.empty() || ascii_lower(item.id).contains(loweredQuery) ||
           ascii_lower(item.localizedName).contains(loweredQuery);
}

/**
 * @brief 筛选目录中匹配搜索词的物品。
 * @param[in] catalog 要筛选的物品目录。
 * @param[in] query 搜索文本；为空时返回所有物品。
 * @return 指向匹配物品的非拥有指针；仅在 `catalog.items` 未发生修改或析构时有效。
 */
[[nodiscard]] inline auto filter_items(const ItemCatalogSnapshot& catalog,
                                       const std::string_view query)
    -> std::vector<const ItemOption*> {
    std::vector<const ItemOption*> filtered;
    filtered.reserve(catalog.items.size());
    for (const auto& item : catalog.items) {
        if (matches_item(item, query)) {
            filtered.push_back(&item);
        }
    }
    return filtered;
}

/**
 * @brief 整理原始物品列表并建立展示标签索引。
 * @param[in] items 待整理的物品值列表。
 * @return 忽略空 ID、按 ID 去重且优先保留非空本地化名、按最终标签稳定排序，并生成
 *         `labelsById` 的目录快照。
 */
[[nodiscard]] inline auto make_item_catalog(std::vector<ItemOption> items) -> ItemCatalogSnapshot {
    std::vector<ItemOption> uniqueItems;
    uniqueItems.reserve(items.size());
    std::unordered_map<std::string, std::size_t> indexesById;
    indexesById.reserve(items.size());

    for (auto& item : items) {
        if (item.id.empty()) {
            continue;
        }
        const auto [found, inserted] = indexesById.emplace(item.id, uniqueItems.size());
        if (inserted) {
            uniqueItems.push_back(std::move(item));
            continue;
        }
        auto& existing = uniqueItems[found->second];
        if (existing.localizedName.empty() && !item.localizedName.empty()) {
            existing.localizedName = std::move(item.localizedName);
        }
    }

    std::stable_sort(uniqueItems.begin(), uniqueItems.end(),
                     [](const ItemOption& left, const ItemOption& right) {
                         return item_label(left) < item_label(right);
                     });

    ItemCatalogSnapshot catalog{.items = std::move(uniqueItems)};
    catalog.labelsById.reserve(catalog.items.size());
    for (const auto& item : catalog.items) {
        catalog.labelsById.emplace(item.id, item_label(item));
    }
    return catalog;
}
}  // namespace item_catalog
