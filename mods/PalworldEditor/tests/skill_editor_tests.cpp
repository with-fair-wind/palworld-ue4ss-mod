#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <items/item_catalog.hpp>
#include <skills/active_skill_definitions.hpp>
#include <skills/selected_target_state.hpp>
#include <skills/skill_catalog.hpp>
#include <skills/skill_editor_service.hpp>
#include <skills/world_session_state.hpp>

namespace {
auto failures = 0;

void check(const bool condition, const char* expression, const int line) {
    if (!condition) {
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++failures;
    }
}
}  // namespace

#define CHECK(expression) check((expression), #expression, __LINE__)

void test_skill_catalog_search_and_labels() {
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

void test_skill_catalog_filter_and_deduplicate() {
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

void test_active_skill_definitions_are_unique_and_known_values_match() {
    const auto definitions = skill_editor::active_skill_definitions();
    CHECK(!definitions.empty());

    std::unordered_set<std::uint16_t> values;
    std::unordered_set<std::string_view> ids;
    for (const auto& definition : definitions) {
        CHECK(definition.value != 0);
        CHECK(!definition.id.empty());
        CHECK(definition.id != "None");
        CHECK(definition.id != "MAX");
        CHECK(values.insert(definition.value).second);
        CHECK(ids.insert(definition.id).second);
    }

    CHECK(skill_editor::find_active_skill_id(1) == std::optional<std::string_view>{"Human_Punch"});
    CHECK(skill_editor::find_active_skill_id(15) ==
          std::optional<std::string_view>{"Unique_Boar_Tackle"});
    CHECK(skill_editor::find_active_skill_id(22) == std::optional<std::string_view>{"AirCanon"});
    CHECK(skill_editor::find_active_skill_id(124) == std::optional<std::string_view>{"MudShot"});
    CHECK(!skill_editor::find_active_skill_id(0).has_value());
    CHECK(skill_editor::active_skill_id_or_numeric(15) == "Unique_Boar_Tackle");
    CHECK(skill_editor::active_skill_id_or_numeric(124) == "MudShot");
    CHECK(skill_editor::active_skill_id_or_numeric(65535) == "65535");
}

void test_active_skill_options_use_runtime_localization_with_raw_id_fallback() {
    constexpr std::array definitions{
        skill_editor::ActiveSkillDefinition{.value = 15, .id = "Unique_Boar_Tackle"},
        skill_editor::ActiveSkillDefinition{.value = 124, .id = "MudShot"},
    };

    const auto options = skill_editor::make_active_skill_options(
        definitions, [](const skill_editor::ActiveSkillDefinition& definition) {
            return definition.value == 15 ? std::string{"野猪突进"} : std::string{};
        });

    CHECK(options.size() == 2);
    CHECK(options[0].id == "Unique_Boar_Tackle");
    CHECK(options[0].localizedName == "野猪突进");
    CHECK(options[0].activeValue == std::optional<std::uint16_t>{std::uint16_t{15}});
    CHECK(skill_editor::skill_label(options[0]) == "野猪突进 [Unique_Boar_Tackle]");
    CHECK(options[1].id == "MudShot");
    CHECK(options[1].localizedName.empty());
    CHECK(skill_editor::skill_label(options[1]) == "MudShot");
}

void test_skill_catalog_refresh_merges_sections_independently() {
    const skill_editor::SkillCatalogSnapshot previous{
        .passive =
            {
                .skills = {{.id = "Passive_Old", .localizedName = "旧被动"}},
                .ready = true,
            },
        .active =
            {
                .skills = {{.id = "OldActive", .activeValue = std::uint16_t{1}}},
                .ready = true,
            },
    };
    const skill_editor::SkillCatalogSnapshot refreshed{
        .passive =
            {
                .skills = {{.id = "Passive_New", .localizedName = "新被动"}},
                .ready = true,
            },
        .active = {.error = "active refresh failed"},
    };

    const auto merged = skill_editor::with_catalog_fallback(previous, refreshed);
    CHECK(merged.passive.ready);
    CHECK(merged.passive.skills.size() == 1);
    CHECK(merged.passive.skills[0].id == "Passive_New");
    CHECK(merged.passive.error.empty());
    CHECK(merged.active.ready);
    CHECK(merged.active.skills.size() == 1);
    CHECK(merged.active.skills[0].id == "OldActive");
    CHECK(merged.active.error == "active refresh failed");
}

void test_skill_catalog_first_partial_load_keeps_available_section() {
    const skill_editor::SkillCatalogSnapshot refreshed{
        .passive =
            {
                .skills = {{.id = "Passive_Swift", .localizedName = "神速"}},
                .ready = true,
            },
        .active = {.error = "active unavailable"},
    };

    const auto merged = skill_editor::with_catalog_fallback({}, refreshed);
    CHECK(merged.passive.ready);
    CHECK(merged.passive.skills.size() == 1);
    CHECK(!merged.active.ready);
    CHECK(merged.active.skills.empty());
    CHECK(merged.active.error == "active unavailable");
}

void test_partial_catalog_is_not_ready_for_editing() {
    skill_editor::SkillCatalogSnapshot catalog{
        .passive =
            {
                .skills = {{.id = "PAL_rude"}},
                .ready = false,
            },
        .active =
            {
                .skills = {{.id = "Unique_Boar_Tackle", .activeValue = std::uint16_t{15}}},
                .ready = true,
            },
        .runtimeReady = false,
    };

    CHECK(!skill_editor::catalog_is_ready_for_editing(catalog));
    catalog.passive.ready = true;
    CHECK(!skill_editor::catalog_is_ready_for_editing(catalog));
    catalog.runtimeReady = true;
    CHECK(skill_editor::catalog_is_ready_for_editing(catalog));
}

void test_catalog_fallback_preserves_established_runtime_readiness() {
    const skill_editor::SkillCatalogSnapshot previous{
        .passive =
            {
                .skills = {{.id = "PAL_rude"}},
                .ready = true,
            },
        .active =
            {
                .skills = {{.id = "Unique_Boar_Tackle", .activeValue = std::uint16_t{15}}},
                .ready = true,
            },
        .runtimeReady = true,
    };
    const skill_editor::SkillCatalogSnapshot failed{
        .passive = {.error = "passive unavailable"},
        .active = {.error = "active unavailable"},
        .runtimeReady = false,
    };

    const auto merged = skill_editor::with_catalog_fallback(previous, failed);
    CHECK(merged.runtimeReady);
    CHECK(skill_editor::catalog_is_ready_for_editing(merged));
}

void test_catalog_refresh_scheduler_throttles_automatic_retries() {
    using namespace std::chrono_literals;
    skill_editor::SkillCatalogRefreshScheduler scheduler{2s};
    const auto start = skill_editor::SkillCatalogRefreshScheduler::time_point{};

    CHECK(scheduler.should_refresh(false, false, start));
    CHECK(!scheduler.should_refresh(false, false, start + 1s));
    CHECK(scheduler.should_refresh(false, false, start + 2s));
    CHECK(!scheduler.should_refresh(false, true, start + 4s));
}

void test_catalog_refresh_scheduler_honors_manual_refresh_immediately() {
    using namespace std::chrono_literals;
    skill_editor::SkillCatalogRefreshScheduler scheduler{2s};
    const auto start = skill_editor::SkillCatalogRefreshScheduler::time_point{};

    CHECK(scheduler.should_refresh(false, false, start));
    CHECK(scheduler.should_refresh(true, false, start + 100ms));
    CHECK(scheduler.should_refresh(true, true, start + 200ms));
}

void test_item_catalog_labels_and_search() {
    const item_catalog::ItemOption localized{.id = "PalSphere", .localizedName = "帕鲁球"};
    const item_catalog::ItemOption fallback{.id = "UnknownItem"};

    CHECK(item_catalog::item_label(localized) == "帕鲁球 [PalSphere]");
    CHECK(item_catalog::item_label(fallback) == "UnknownItem");
    CHECK(item_catalog::matches_item(localized, "帕鲁"));
    CHECK(item_catalog::matches_item(localized, "palsphere"));
    CHECK(!item_catalog::matches_item(localized, "木材"));
}

void test_item_catalog_deduplicates_indexes_and_sorts() {
    auto catalog = item_catalog::make_item_catalog({
        {.id = "Wood", .localizedName = "Zulu"},
        {.id = "PalSphere"},
        {.id = "PalSphere", .localizedName = "Alpha"},
        {.id = "Wood", .localizedName = "Repeated"},
    });

    CHECK(catalog.items.size() == 2);
    CHECK(item_catalog::item_label(catalog, "PalSphere") == "Alpha [PalSphere]");
    CHECK(item_catalog::item_label(catalog, "Missing") == "Missing");
    CHECK(catalog.items[0].id == "PalSphere");

    const auto filtered = item_catalog::filter_items(catalog, "alpha");
    CHECK(filtered.size() == 1);
    CHECK(filtered[0]->id == "PalSphere");
}

class FakeSkillGateway final : public skill_editor::ISkillGateway {
public:
    bool valid{true};
    skill_editor::SkillState state;
    std::deque<bool> addOutcomes;
    std::deque<bool> removeOutcomes;
    std::deque<bool> rewriteOutcomes;
    std::deque<std::optional<std::vector<skill_editor::ActiveSkill>>> rewriteStates;
    std::vector<std::string> calls;

    auto is_valid(const skill_editor::SkillTarget) const -> bool override {
        return valid;
    }

    auto read_state(const skill_editor::SkillTarget) -> skill_editor::SkillState override {
        calls.emplace_back("read");
        return state;
    }

    auto add_passive(const skill_editor::SkillTarget, const std::string_view id) -> bool override {
        calls.emplace_back("add:" + std::string(id));
        const bool succeeds = pop_or_default(addOutcomes, true);
        if (succeeds) {
            state.passiveIds.emplace_back(id);
        }
        return succeeds;
    }

    auto remove_passive(const skill_editor::SkillTarget, const std::string_view id)
        -> bool override {
        calls.emplace_back("remove:" + std::string(id));
        const bool succeeds = pop_or_default(removeOutcomes, true);
        if (succeeds) {
            std::erase(state.passiveIds, id);
        }
        return succeeds;
    }

    auto rewrite_active(const skill_editor::SkillTarget,
                        const std::span<const skill_editor::ActiveSkill> skills) -> bool override {
        calls.emplace_back("rewrite");
        const bool succeeds = pop_or_default(rewriteOutcomes, true);
        if (!rewriteStates.empty()) {
            auto replacement = std::move(rewriteStates.front());
            rewriteStates.pop_front();
            if (replacement.has_value()) {
                state.activeSkills = std::move(*replacement);
            }
        } else if (succeeds) {
            state.activeSkills.assign(skills.begin(), skills.end());
        }
        return succeeds;
    }

private:
    static auto pop_or_default(std::deque<bool>& values, const bool fallback) -> bool {
        if (values.empty()) {
            return fallback;
        }
        const bool result = values.front();
        values.pop_front();
        return result;
    }
};

auto passive_request(const skill_editor::SkillEditOperation operation, std::string oldId = {},
                     std::string newId = {}) -> skill_editor::SkillEditRequest {
    return {
        .target = 0x1234,
        .kind = skill_editor::SkillKind::passive,
        .operation = operation,
        .oldPassiveId = std::move(oldId),
        .newPassiveId = std::move(newId),
    };
}

void test_passive_edits_validate_target_and_limits() {
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

void test_passive_add_remove_and_replace_reread_state() {
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

void test_passive_replace_rolls_back_on_failure() {
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

auto active_request(const skill_editor::SkillEditOperation operation, const std::size_t slot,
                    std::optional<skill_editor::ActiveSkill> skill = std::nullopt)
    -> skill_editor::SkillEditRequest {
    return {
        .target = 0x1234,
        .kind = skill_editor::SkillKind::active,
        .operation = operation,
        .activeSlot = slot,
        .newActiveSkill = std::move(skill),
    };
}

void test_active_edits_validate_three_compact_slots() {
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

void test_active_add_replace_and_remove_preserve_order() {
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
          std::vector<skill_editor::ActiveSkill>(
              {{1, "FireBall"}, {4, "IceMissile"}, {3, "WindCutter"}}));

    result = skill_editor::execute_skill_edit(
        gateway, active_request(skill_editor::SkillEditOperation::remove, 1));
    CHECK(result.status == skill_editor::SkillEditStatus::succeeded);
    CHECK(result.state.activeSkills ==
          std::vector<skill_editor::ActiveSkill>({{1, "FireBall"}, {3, "WindCutter"}}));
}

void test_active_edit_rolls_back_complete_original_sequence() {
    FakeSkillGateway gateway;
    const std::vector<skill_editor::ActiveSkill> original{
        {1, "FireBall"}, {2, "WaterGun"}, {3, "WindCutter"}};
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
    CHECK(result.state.activeSkills == std::vector<skill_editor::ActiveSkill>({{9, "Wrong"}}));
}

void test_skill_edit_queue_is_fifo() {
    skill_editor::SkillEditQueue queue;
    queue.push({.target = 1});
    queue.push({.target = 2});
    queue.push({.target = 3});

    CHECK(queue.size() == 3);

    const auto first = queue.try_pop();
    const auto second = queue.try_pop();
    const auto third = queue.try_pop();
    CHECK(first.has_value() && first->target == 1);
    CHECK(second.has_value() && second->target == 2);
    CHECK(third.has_value() && third->target == 3);
    CHECK(!queue.try_pop().has_value());
    CHECK(queue.size() == 0);
}

void test_skill_edit_queue_can_discard_all_pending_requests() {
    skill_editor::SkillEditQueue queue;
    queue.push({.targetGeneration = 1});
    queue.push({.targetGeneration = 1});
    queue.push({.targetGeneration = 1});

    CHECK(queue.clear() == 3);
    CHECK(queue.size() == 0);
    CHECK(!queue.try_pop().has_value());
}

auto identity(const std::uint32_t value) -> skill_editor::TargetIdentity {
    return {.instanceId = {value, value + 1, value + 2, value + 3}};
}

void test_target_requires_explicit_confirmation() {
    skill_editor::SelectedTargetState state;
    const skill_editor::SelectedTargetObservation observed{
        .identity = identity(10),
        .name = "Boar",
    };

    CHECK(!state.is_selected());
    CHECK(!state.matches_current(observed));
    CHECK(!state.is_selected());

    CHECK(state.confirm(observed));
    CHECK(state.is_selected());
    CHECK(state.matches_current(observed));
    CHECK(state.current().identity == observed.identity);
    CHECK(state.current().name == "Boar");
    CHECK(state.generation() == 1);
}

void test_world_session_transition_requires_reconfirmation() {
    skill_editor::WorldSessionState session;
    CHECK(session.can_access_unreal());
    CHECK(session.confirm_target());
    CHECK(session.is_target_confirmed());

    session.begin_transition();
    CHECK(session.generation() == 1);
    CHECK(!session.can_access_unreal());
    CHECK(!session.is_target_confirmed());
    CHECK(!session.request_targets_current_world(0));
    CHECK(!session.request_is_current(0));

    session.finish_transition();
    CHECK(session.can_access_unreal());
    CHECK(!session.is_target_confirmed());
    CHECK(session.request_targets_current_world(1));
    CHECK(!session.request_is_current(1));

    CHECK(session.confirm_target());
    CHECK(session.request_is_current(1));
}

void test_world_session_cannot_confirm_during_transition() {
    skill_editor::WorldSessionState session;
    session.begin_transition();
    CHECK(!session.confirm_target());
    CHECK(!session.is_target_confirmed());
}

void test_observations_do_not_replace_or_clear_explicit_target() {
    skill_editor::SelectedTargetState state;
    CHECK(state.confirm({.identity = identity(10), .name = "Boar"}));
    const auto selectedGeneration = state.generation();

    CHECK(state.matches_current({.identity = identity(10), .name = "Boar"}));
    CHECK(!state.matches_current({}));
    CHECK(!state.matches_current({.identity = identity(20), .name = "SheepBall"}));
    CHECK(state.is_selected());
    CHECK(state.current().identity == identity(10));
    CHECK(state.current().name == "Boar");
    CHECK(state.generation() == selectedGeneration);
}

void test_resolution_status_has_actionable_message() {
    using enum skill_editor::SelectedTargetResolutionStatus;
    CHECK(skill_editor::resolution_status_message(holderCandidatesUnavailable) ==
          "未发现队伍 Holder");
    CHECK(skill_editor::resolution_status_message(holderOwnerPawnUnavailable) ==
          "未取得队伍 Holder 的所属玩家角色");
    CHECK(skill_editor::resolution_status_message(holderOwnerControllerUnavailable) ==
          "未取得队伍 Holder 的所属控制器");
    CHECK(skill_editor::resolution_status_message(localHolderUnavailable) ==
          "未找到本地玩家的队伍 Holder");
    CHECK(skill_editor::resolution_status_message(localHolderAmbiguous) ==
          "发现多个本地玩家队伍 Holder，已拒绝猜测");
    CHECK(skill_editor::resolution_status_message(getSelectedFunctionUnavailable) ==
          "实际 Holder 类未实现 GetSelectedOtomoID");
    CHECK(skill_editor::resolution_status_message(selectedSlotUnavailable) ==
          "当前高亮队伍槽位没有有效帕鲁");
    CHECK(skill_editor::resolution_status_message(success).empty());
}

struct LocalCandidateProbe {
    bool valid{true};
    bool hasOwnerPawn{true};
    bool hasController{true};
    bool local{};
};

auto select_local_candidate(const std::vector<LocalCandidateProbe*>& candidates) {
    return skill_editor::find_unique_local_candidate(
        candidates,
        [](const LocalCandidateProbe* candidate) {
            return candidate != nullptr && candidate->valid;
        },
        [](LocalCandidateProbe* candidate) {
            return candidate->hasOwnerPawn ? candidate : nullptr;
        },
        [](LocalCandidateProbe* pawn) { return pawn->hasController ? pawn : nullptr; },
        [](const LocalCandidateProbe* controller) { return controller->local; });
}

void test_unique_local_candidate_is_selected() {
    LocalCandidateProbe remote{.local = false};
    LocalCandidateProbe local{.local = true};
    const auto selection = select_local_candidate({&remote, &local});

    CHECK(selection.status == skill_editor::LocalCandidateSelectionStatus::success);
    CHECK(selection.candidate.has_value());
    CHECK(*selection.candidate == &local);
    CHECK(selection.candidateCount == 2);
    CHECK(selection.localCandidateCount == 1);
}

void test_local_candidate_selection_reports_each_unavailable_stage() {
    CHECK(select_local_candidate({}).status ==
          skill_editor::LocalCandidateSelectionStatus::noCandidates);

    LocalCandidateProbe noPawn{.hasOwnerPawn = false};
    CHECK(select_local_candidate({&noPawn}).status ==
          skill_editor::LocalCandidateSelectionStatus::ownerPawnUnavailable);

    LocalCandidateProbe noController{.hasController = false};
    CHECK(select_local_candidate({&noController}).status ==
          skill_editor::LocalCandidateSelectionStatus::ownerControllerUnavailable);

    LocalCandidateProbe remote{.local = false};
    CHECK(select_local_candidate({&remote}).status ==
          skill_editor::LocalCandidateSelectionStatus::localCandidateUnavailable);
}

void test_multiple_local_candidates_are_rejected() {
    LocalCandidateProbe first{.local = true};
    LocalCandidateProbe second{.local = true};
    const auto selection = select_local_candidate({&first, &second});

    CHECK(selection.status ==
          skill_editor::LocalCandidateSelectionStatus::ambiguousLocalCandidates);
    CHECK(!selection.candidate.has_value());
    CHECK(selection.localCandidateCount == 2);
}

void test_stale_generation_never_reaches_apply_callback() {
    skill_editor::SelectedTargetState state;
    skill_editor::WorldSessionState session;
    const skill_editor::SelectedTargetObservation observed{
        .identity = identity(10),
        .name = "Boar",
    };
    CHECK(state.confirm(observed));
    CHECK(session.confirm_target());

    int applyCalls = 0;
    skill_editor::SkillTarget appliedTarget = 0;
    const auto apply = [&applyCalls,
                        &appliedTarget](const skill_editor::SkillEditRequest& request) {
        ++applyCalls;
        appliedTarget = request.target;
        return skill_editor::SkillEditResult{
            .status = skill_editor::SkillEditStatus::succeeded,
        };
    };

    const auto accepted = skill_editor::apply_if_target_is_current(
        {.targetGeneration = state.generation(), .worldGeneration = session.generation()}, state,
        observed, 0x2000, session, apply);
    CHECK(accepted.has_value());
    CHECK(applyCalls == 1);
    CHECK(appliedTarget == 0x2000);

    const auto stale = skill_editor::apply_if_target_is_current(
        {.targetGeneration = state.generation() + 1, .worldGeneration = session.generation()}, state,
        observed, 0x2000, session, apply);
    CHECK(!stale.has_value());
    CHECK(applyCalls == 1);

    session.begin_transition();
    session.finish_transition();
    const auto staleWorld = skill_editor::apply_if_target_is_current(
        {.targetGeneration = state.generation(), .worldGeneration = 0}, state, observed, 0x2000,
        session, apply);
    CHECK(!staleWorld.has_value());
    CHECK(applyCalls == 1);

    const auto unconfirmedWorld = skill_editor::apply_if_target_is_current(
        {.targetGeneration = state.generation(), .worldGeneration = session.generation()}, state,
        observed, 0x2000, session, apply);
    CHECK(!unconfirmedWorld.has_value());
    CHECK(applyCalls == 1);
}

auto main() -> int {
    test_skill_catalog_search_and_labels();
    test_skill_catalog_filter_and_deduplicate();
    test_active_skill_definitions_are_unique_and_known_values_match();
    test_active_skill_options_use_runtime_localization_with_raw_id_fallback();
    test_skill_catalog_refresh_merges_sections_independently();
    test_skill_catalog_first_partial_load_keeps_available_section();
    test_partial_catalog_is_not_ready_for_editing();
    test_catalog_fallback_preserves_established_runtime_readiness();
    test_catalog_refresh_scheduler_throttles_automatic_retries();
    test_catalog_refresh_scheduler_honors_manual_refresh_immediately();
    test_item_catalog_labels_and_search();
    test_item_catalog_deduplicates_indexes_and_sorts();
    test_passive_edits_validate_target_and_limits();
    test_passive_add_remove_and_replace_reread_state();
    test_passive_replace_rolls_back_on_failure();
    test_active_edits_validate_three_compact_slots();
    test_active_add_replace_and_remove_preserve_order();
    test_active_edit_rolls_back_complete_original_sequence();
    test_skill_edit_queue_is_fifo();
    test_skill_edit_queue_can_discard_all_pending_requests();
    test_target_requires_explicit_confirmation();
    test_world_session_transition_requires_reconfirmation();
    test_world_session_cannot_confirm_during_transition();
    test_observations_do_not_replace_or_clear_explicit_target();
    test_resolution_status_has_actionable_message();
    test_unique_local_candidate_is_selected();
    test_local_candidate_selection_reports_each_unavailable_stage();
    test_multiple_local_candidates_are_rejected();
    test_stale_generation_never_reaches_apply_callback();
    return failures == 0 ? 0 : 1;
}
