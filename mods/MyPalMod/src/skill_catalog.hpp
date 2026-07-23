#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace skill_editor
{
struct SkillOption
{
    std::string id;
    std::string localizedName;
    std::optional<std::uint16_t> activeValue;
};

[[nodiscard]] inline auto ascii_lower(const std::string_view value) -> std::string
{
    std::string result(value);
    for (auto& character : result)
    {
        if (character >= 'A' && character <= 'Z')
        {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return result;
}

[[nodiscard]] inline auto skill_label(const SkillOption& option) -> std::string
{
    if (option.localizedName.empty())
    {
        return option.id;
    }
    return option.localizedName + " [" + option.id + "]";
}

[[nodiscard]] inline auto matches_skill(const SkillOption& option, const std::string_view query) -> bool
{
    if (query.empty())
    {
        return true;
    }

    const auto normalizedQuery = ascii_lower(query);
    return ascii_lower(option.id).contains(normalizedQuery) ||
           ascii_lower(option.localizedName).contains(normalizedQuery);
}

[[nodiscard]] inline auto deduplicate_skills(std::vector<SkillOption> options) -> std::vector<SkillOption>
{
    std::unordered_set<std::string> seen;
    std::vector<SkillOption> result;
    result.reserve(options.size());
    for (auto& option : options)
    {
        if (!option.id.empty() && seen.insert(option.id).second)
        {
            result.push_back(std::move(option));
        }
    }
    return result;
}

[[nodiscard]] inline auto filter_skills(
    const std::span<const SkillOption> options,
    const std::string_view query,
    const std::unordered_set<std::string>& excludedIds) -> std::vector<SkillOption>
{
    std::vector<SkillOption> result;
    for (const auto& option : options)
    {
        if (!excludedIds.contains(option.id) && matches_skill(option, query))
        {
            result.push_back(option);
        }
    }
    return result;
}
}
