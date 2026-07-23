#include "skill_catalog.hpp"

#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>

namespace
{
auto failures = 0;

void check(const bool condition, const char* expression, const int line)
{
    if (!condition)
    {
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++failures;
    }
}
}

#define CHECK(expression) check((expression), #expression, __LINE__)

void test_skill_catalog_search_and_labels()
{
    const std::vector<skill_editor::SkillOption> options{
        {.id = "Passive_Swift", .localizedName = "神速"},
        {.id = "Passive_Workaholic", .localizedName = "工作狂"},
        {.id = "Passive_Unknown"},
    };

    CHECK(skill_editor::matches_skill(options[0], "神速"));
    CHECK(skill_editor::matches_skill(options[0], "passive_swift"));
    CHECK(skill_editor::matches_skill(options[1], "工作"));
    CHECK(!skill_editor::matches_skill(options[1], "神速"));
    CHECK(skill_editor::skill_label(options[0]) == "神速 [Passive_Swift]");
    CHECK(skill_editor::skill_label(options[2]) == "Passive_Unknown");
}

void test_skill_catalog_filter_and_deduplicate()
{
    const std::vector<skill_editor::SkillOption> options{
        {.id = "Passive_Swift", .localizedName = "神速"},
        {.id = "Passive_Workaholic", .localizedName = "工作狂"},
        {.id = "Passive_Swift", .localizedName = "重复神速"},
    };

    const auto unique = skill_editor::deduplicate_skills(options);
    CHECK(unique.size() == 2);
    CHECK(unique[0].localizedName == "神速");

    const auto visible = skill_editor::filter_skills(
        unique, "passive", std::unordered_set<std::string>{"Passive_Swift"});
    CHECK(visible.size() == 1);
    CHECK(visible[0].id == "Passive_Workaholic");
}

auto main() -> int
{
    test_skill_catalog_search_and_labels();
    test_skill_catalog_filter_and_deduplicate();
    return failures == 0 ? 0 : 1;
}
