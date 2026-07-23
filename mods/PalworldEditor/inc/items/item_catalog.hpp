#pragma once

#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace item_catalog {
struct ItemOption {
    std::string id;
    std::string localizedName;
};

struct ItemCatalogSnapshot {
    std::vector<ItemOption> items;
    std::unordered_map<std::string, std::string> labelsById;
};

[[nodiscard]] inline auto ascii_lower(std::string_view value) -> std::string {
    std::string lowered(value);
    for (auto& character : lowered) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return lowered;
}

[[nodiscard]] inline auto item_label(const ItemOption& item) -> std::string {
    if (item.localizedName.empty()) {
        return item.id;
    }
    return item.localizedName + " [" + item.id + ']';
}

[[nodiscard]] inline auto item_label(const ItemCatalogSnapshot& catalog,
                                     const std::string_view id) -> std::string {
    const auto found = catalog.labelsById.find(std::string(id));
    return found == catalog.labelsById.end() ? std::string(id) : found->second;
}

[[nodiscard]] inline auto matches_item(const ItemOption& item, const std::string_view query)
    -> bool {
    const auto loweredQuery = ascii_lower(query);
    return loweredQuery.empty() || ascii_lower(item.id).contains(loweredQuery) ||
           ascii_lower(item.localizedName).contains(loweredQuery);
}

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

[[nodiscard]] inline auto make_item_catalog(std::vector<ItemOption> items)
    -> ItemCatalogSnapshot {
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
