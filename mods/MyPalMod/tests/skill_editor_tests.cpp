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

void test_skill_catalog_refresh_keeps_last_success()
{
    skill_editor::SkillCatalogSnapshot previous{
        .passiveSkills = {{.id = "Passive_Swift", .localizedName = "神速"}},
        .activeSkills = {
            {.id = "FireBall", .localizedName = "火球", .activeValue = std::uint16_t{1}}},
        .ready = true,
    };
    skill_editor::SkillCatalogSnapshot failed{.error = "runtime lookup failed"};

    const auto fallback = skill_editor::with_catalog_fallback(previous, failed);
    CHECK(fallback.ready);
    CHECK(fallback.passiveSkills.size() == 1);
    CHECK(fallback.activeSkills.size() == 1);
    CHECK(fallback.error == "runtime lookup failed");

    const auto unavailable = skill_editor::with_catalog_fallback({}, failed);
    CHECK(!unavailable.ready);
    CHECK(unavailable.passiveSkills.empty());
    CHECK(unavailable.error == "runtime lookup failed");
}

class FakeSkillGateway final : public skill_editor::ISkillGateway
{
public:
    bool valid{true};
    skill_editor::SkillState state;
    std::deque<bool> addOutcomes;
    std::deque<bool> removeOutcomes;
    std::deque<bool> rewriteOutcomes;
    std::deque<std::optional<std::vector<skill_editor::ActiveSkill>>> rewriteStates;
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
        const std::span<const skill_editor::ActiveSkill> skills) -> bool override
    {
        calls.emplace_back("rewrite");
        const bool succeeds = pop_or_default(rewriteOutcomes, true);
        if (!rewriteStates.empty())
        {
            auto replacement = std::move(rewriteStates.front());
            rewriteStates.pop_front();
            if (replacement.has_value())
            {
                state.activeSkills = std::move(*replacement);
            }
        }
        else if (succeeds)
        {
            state.activeSkills.assign(skills.begin(), skills.end());
        }
        return succeeds;
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

auto active_request(
    const skill_editor::SkillEditOperation operation,
    const std::size_t slot,
    std::optional<skill_editor::ActiveSkill> skill = std::nullopt) -> skill_editor::SkillEditRequest
{
    return {
        .target = 0x1234,
        .kind = skill_editor::SkillKind::active,
        .operation = operation,
        .activeSlot = slot,
        .newActiveSkill = std::move(skill),
    };
}

void test_active_edits_validate_three_compact_slots()
{
    FakeSkillGateway gateway;
    gateway.state.activeSkills = {{1, "FireBall"}};

    auto result = skill_editor::execute_skill_edit(
        gateway, active_request(skill_editor::SkillEditOperation::add, 2, {{2, "WaterGun"}}));
    CHECK(result.status == skill_editor::SkillEditStatus::rejected);

    result = skill_editor::execute_skill_edit(
        gateway, active_request(skill_editor::SkillEditOperation::replace, 1, {{2, "WaterGun"}}));
    CHECK(result.status == skill_editor::SkillEditStatus::rejected);

    result = skill_editor::execute_skill_edit(
        gateway, active_request(skill_editor::SkillEditOperation::remove, 3));
    CHECK(result.status == skill_editor::SkillEditStatus::rejected);

    gateway.state.activeSkills = {{1, "FireBall"}, {2, "WaterGun"}, {3, "WindCutter"}};
    result = skill_editor::execute_skill_edit(
        gateway, active_request(skill_editor::SkillEditOperation::add, 3, {{4, "IceMissile"}}));
    CHECK(result.status == skill_editor::SkillEditStatus::rejected);

    result = skill_editor::execute_skill_edit(
        gateway, active_request(skill_editor::SkillEditOperation::replace, 1, {{3, "WindCutter"}}));
    CHECK(result.status == skill_editor::SkillEditStatus::rejected);
}

void test_active_add_replace_and_remove_preserve_order()
{
    FakeSkillGateway gateway;
    gateway.state.activeSkills = {{1, "FireBall"}};

    auto result = skill_editor::execute_skill_edit(
        gateway, active_request(skill_editor::SkillEditOperation::add, 1, {{2, "WaterGun"}}));
    CHECK(result.status == skill_editor::SkillEditStatus::succeeded);
    CHECK(result.state.activeSkills ==
          std::vector<skill_editor::ActiveSkill>({{1, "FireBall"}, {2, "WaterGun"}}));

    gateway.state.activeSkills.push_back({3, "WindCutter"});
    result = skill_editor::execute_skill_edit(
        gateway, active_request(skill_editor::SkillEditOperation::replace, 1, {{4, "IceMissile"}}));
    CHECK(result.status == skill_editor::SkillEditStatus::succeeded);
    CHECK(result.state.activeSkills ==
          std::vector<skill_editor::ActiveSkill>({{1, "FireBall"}, {4, "IceMissile"}, {3, "WindCutter"}}));

    result = skill_editor::execute_skill_edit(
        gateway, active_request(skill_editor::SkillEditOperation::remove, 1));
    CHECK(result.status == skill_editor::SkillEditStatus::succeeded);
    CHECK(result.state.activeSkills ==
          std::vector<skill_editor::ActiveSkill>({{1, "FireBall"}, {3, "WindCutter"}}));
}

void test_active_edit_rolls_back_complete_original_sequence()
{
    FakeSkillGateway gateway;
    const std::vector<skill_editor::ActiveSkill> original{{1, "FireBall"}, {2, "WaterGun"}, {3, "WindCutter"}};
    gateway.state.activeSkills = original;
    gateway.rewriteStates = {
        std::vector<skill_editor::ActiveSkill>{{9, "Wrong"}},
        original,
    };

    auto result = skill_editor::execute_skill_edit(
        gateway, active_request(skill_editor::SkillEditOperation::replace, 1, {{4, "IceMissile"}}));
    CHECK(result.status == skill_editor::SkillEditStatus::rolledBack);
    CHECK(result.state.activeSkills == original);

    gateway.state.activeSkills = original;
    gateway.rewriteStates = {
        std::vector<skill_editor::ActiveSkill>{{9, "Wrong"}},
        std::nullopt,
    };
    gateway.rewriteOutcomes = {true, false};
    result = skill_editor::execute_skill_edit(
        gateway, active_request(skill_editor::SkillEditOperation::replace, 1, {{4, "IceMissile"}}));
    CHECK(result.status == skill_editor::SkillEditStatus::rollbackFailed);
    CHECK(result.state.activeSkills ==
          std::vector<skill_editor::ActiveSkill>({{9, "Wrong"}}));
}

void test_skill_edit_queue_is_fifo()
{
    skill_editor::SkillEditQueue queue;
    queue.push({.target = 1});
    queue.push({.target = 2});
    queue.push({.target = 3});

    CHECK(queue.size() == 3);
    CHECK(queue.contains_target(2));
    CHECK(!queue.contains_target(9));

    const auto first = queue.try_pop();
    const auto second = queue.try_pop();
    const auto third = queue.try_pop();
    CHECK(first.has_value() && first->target == 1);
    CHECK(second.has_value() && second->target == 2);
    CHECK(third.has_value() && third->target == 3);
    CHECK(!queue.try_pop().has_value());
    CHECK(queue.size() == 0);
}

auto main() -> int
{
    test_skill_catalog_search_and_labels();
    test_skill_catalog_filter_and_deduplicate();
    test_skill_catalog_refresh_keeps_last_success();
    test_passive_edits_validate_target_and_limits();
    test_passive_add_remove_and_replace_reread_state();
    test_passive_replace_rolls_back_on_failure();
    test_active_edits_validate_three_compact_slots();
    test_active_add_replace_and_remove_preserve_order();
    test_active_edit_rolls_back_complete_original_sequence();
    test_skill_edit_queue_is_fifo();
    return failures == 0 ? 0 : 1;
}
