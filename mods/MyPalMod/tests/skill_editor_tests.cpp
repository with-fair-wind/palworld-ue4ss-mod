#include "skill_catalog.hpp"
#include "skill_editor_service.hpp"

#include <algorithm>
#include <deque>
#include <iostream>
#include <span>
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

class FakeSkillGateway final : public skill_editor::ISkillGateway
{
public:
    bool valid{true};
    skill_editor::SkillState state;
    std::deque<bool> addOutcomes;
    std::deque<bool> removeOutcomes;
    std::vector<std::string> calls;

    auto is_valid(const skill_editor::SkillTarget) const -> bool override
    {
        return valid;
    }

    auto read_state(const skill_editor::SkillTarget) -> skill_editor::SkillState override
    {
        calls.emplace_back("read");
        return state;
    }

    auto add_passive(const skill_editor::SkillTarget, const std::string_view id) -> bool override
    {
        calls.emplace_back("add:" + std::string(id));
        const bool succeeds = pop_or_default(addOutcomes, true);
        if (succeeds)
        {
            state.passiveIds.emplace_back(id);
        }
        return succeeds;
    }

    auto remove_passive(const skill_editor::SkillTarget, const std::string_view id) -> bool override
    {
        calls.emplace_back("remove:" + std::string(id));
        const bool succeeds = pop_or_default(removeOutcomes, true);
        if (succeeds)
        {
            std::erase(state.passiveIds, id);
        }
        return succeeds;
    }

    auto rewrite_active(
        const skill_editor::SkillTarget,
        const std::span<const skill_editor::ActiveSkill>) -> bool override
    {
        calls.emplace_back("rewrite");
        return false;
    }

private:
    static auto pop_or_default(std::deque<bool>& values, const bool fallback) -> bool
    {
        if (values.empty())
        {
            return fallback;
        }
        const bool result = values.front();
        values.pop_front();
        return result;
    }
};

auto passive_request(
    const skill_editor::SkillEditOperation operation,
    std::string oldId = {},
    std::string newId = {}) -> skill_editor::SkillEditRequest
{
    return {
        .target = 0x1234,
        .kind = skill_editor::SkillKind::passive,
        .operation = operation,
        .oldPassiveId = std::move(oldId),
        .newPassiveId = std::move(newId),
    };
}

void test_passive_edits_validate_target_and_limits()
{
    FakeSkillGateway gateway;
    gateway.valid = false;
    auto result = skill_editor::execute_skill_edit(
        gateway, passive_request(skill_editor::SkillEditOperation::add, {}, "Passive_Swift"));
    CHECK(result.status == skill_editor::SkillEditStatus::invalidTarget);
    CHECK(gateway.calls.empty());

    gateway.valid = true;
    gateway.state.passiveIds = {"A", "B", "C", "D"};
    result = skill_editor::execute_skill_edit(
        gateway, passive_request(skill_editor::SkillEditOperation::add, {}, "E"));
    CHECK(result.status == skill_editor::SkillEditStatus::rejected);
    CHECK(gateway.state.passiveIds.size() == 4);

    gateway.state.passiveIds = {"A"};
    result = skill_editor::execute_skill_edit(
        gateway, passive_request(skill_editor::SkillEditOperation::add, {}, "A"));
    CHECK(result.status == skill_editor::SkillEditStatus::rejected);
}

void test_passive_add_remove_and_replace_reread_state()
{
    FakeSkillGateway gateway;
    gateway.state.passiveIds = {"A", "B"};

    auto result = skill_editor::execute_skill_edit(
        gateway, passive_request(skill_editor::SkillEditOperation::add, {}, "C"));
    CHECK(result.status == skill_editor::SkillEditStatus::succeeded);
    CHECK(result.state.passiveIds.size() == 3);

    result = skill_editor::execute_skill_edit(
        gateway, passive_request(skill_editor::SkillEditOperation::remove, "B"));
    CHECK(result.status == skill_editor::SkillEditStatus::succeeded);
    CHECK(!std::ranges::contains(result.state.passiveIds, "B"));

    result = skill_editor::execute_skill_edit(
        gateway, passive_request(skill_editor::SkillEditOperation::replace, "A", "D"));
    CHECK(result.status == skill_editor::SkillEditStatus::succeeded);
    CHECK(!std::ranges::contains(result.state.passiveIds, "A"));
    CHECK(std::ranges::contains(result.state.passiveIds, "D"));
}

void test_passive_replace_rolls_back_on_failure()
{
    FakeSkillGateway gateway;
    gateway.state.passiveIds = {"A", "B"};
    gateway.addOutcomes = {false, true};

    auto result = skill_editor::execute_skill_edit(
        gateway, passive_request(skill_editor::SkillEditOperation::replace, "A", "C"));
    CHECK(result.status == skill_editor::SkillEditStatus::rolledBack);
    CHECK(result.state.passiveIds == std::vector<std::string>({"B", "A"}));

    gateway.state.passiveIds = {"A", "B"};
    gateway.calls.clear();
    gateway.addOutcomes = {false, false};
    result = skill_editor::execute_skill_edit(
        gateway, passive_request(skill_editor::SkillEditOperation::replace, "A", "C"));
    CHECK(result.status == skill_editor::SkillEditStatus::rollbackFailed);
    CHECK(!std::ranges::contains(result.state.passiveIds, "A"));
}

auto main() -> int
{
    test_skill_catalog_search_and_labels();
    test_skill_catalog_filter_and_deduplicate();
    test_passive_edits_validate_target_and_limits();
    test_passive_add_remove_and_replace_reread_state();
    test_passive_replace_rolls_back_on_failure();
    return failures == 0 ? 0 : 1;
}
