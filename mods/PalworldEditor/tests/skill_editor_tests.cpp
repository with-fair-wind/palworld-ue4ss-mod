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
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <items/item_catalog.hpp>
#include <pal_stats/pal_stat_editor.hpp>
#include <skills/active_skill_definitions.hpp>
#include <skills/pal_resolution_scheduler.hpp>
#include <skills/passive_skill_presets.hpp>
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

void test_passive_skill_categories_follow_runtime_metadata() {
    using enum skill_editor::PassiveSkillCategory;

    CHECK(skill_editor::classify_passive_skill(0, false) == normal);
    CHECK(skill_editor::classify_passive_skill(2, false) == normal);
    CHECK(skill_editor::classify_passive_skill(3, false) == rare);
    CHECK(skill_editor::classify_passive_skill(4, false) == premium);
    CHECK(skill_editor::classify_passive_skill(9, false) == premium);
    CHECK(skill_editor::classify_passive_skill(-1, false) == negative);
    CHECK(skill_editor::classify_passive_skill(-1, true) == legendary);
    CHECK(skill_editor::classify_passive_skill(2, true) == legendary);
}

void test_passive_filter_combines_category_exclusion_and_search() {
    using enum skill_editor::PassiveSkillCategory;

    const std::vector<skill_editor::SkillOption> skills{
        {.id = "Passive_Normal",
         .localizedName = "普通技能",
         .passiveMetadata =
             skill_editor::PassiveSkillMetadata{
                 .rank = 1, .addWorldTreePal = false, .category = normal}},
        {.id = "Passive_Rare",
         .localizedName = "稀有采矿",
         .passiveMetadata =
             skill_editor::PassiveSkillMetadata{
                 .rank = 3, .addWorldTreePal = false, .category = rare}},
        {.id = "Passive_Legend",
         .localizedName = "传说采矿",
         .passiveMetadata =
             skill_editor::PassiveSkillMetadata{
                 .rank = 1, .addWorldTreePal = true, .category = legendary}},
        {.id = "Passive_Unknown", .localizedName = "未知采矿"},
    };
    const std::unordered_set<std::string> equipped{"Passive_Legend"};

    const auto rareSkills = skill_editor::filter_passive_skills(skills, rare, "采矿", equipped);
    CHECK(rareSkills.size() == 1);
    CHECK(rareSkills.front().id == "Passive_Rare");

    const auto allSkills =
        skill_editor::filter_passive_skills(skills, std::nullopt, "Passive_", equipped);
    CHECK(allSkills.size() == 3);
    CHECK(std::ranges::any_of(allSkills, [](const skill_editor::SkillOption& option) {
        return option.id == "Passive_Unknown";
    }));
}

void test_passive_filter_views_preserve_catalog_objects_and_order() {
    using enum skill_editor::PassiveSkillCategory;

    const std::vector<skill_editor::SkillOption> skills{
        {.id = "Normal",
         .localizedName = "普通采矿",
         .passiveMetadata =
             skill_editor::PassiveSkillMetadata{
                 .rank = 1, .addWorldTreePal = false, .category = normal}},
        {.id = "Rare",
         .localizedName = "稀有采矿",
         .passiveMetadata =
             skill_editor::PassiveSkillMetadata{
                 .rank = 3, .addWorldTreePal = false, .category = rare}},
        {.id = "RareExcluded",
         .localizedName = "稀有采矿二",
         .passiveMetadata =
             skill_editor::PassiveSkillMetadata{
                 .rank = 3, .addWorldTreePal = false, .category = rare}},
    };

    const auto visible = skill_editor::filter_passive_skill_views(
        skills, rare, "采矿", std::unordered_set<std::string>{"RareExcluded"});

    CHECK(visible.size() == 1);
    CHECK(visible.front() == &skills[1]);
}

void test_passive_picker_category_change_clears_only_selection() {
    skill_editor::PassiveSkillPickerState state{
        .category = std::nullopt,
        .selected = skill_editor::SkillOption{.id = "Passive_Rare", .localizedName = "稀有采矿"},
    };

    CHECK(state.set_category(skill_editor::PassiveSkillCategory::rare));
    CHECK(state.category == skill_editor::PassiveSkillCategory::rare);
    CHECK(!state.selected.has_value());
    CHECK(!state.set_category(skill_editor::PassiveSkillCategory::rare));
}

void test_passive_classification_job_reuses_success_cache_and_retries_unknowns() {
    using enum skill_editor::PassiveSkillCategory;

    const std::vector<skill_editor::SkillOption> skills{
        {.id = "Cached", .localizedName = "已缓存"},
        {.id = "Retry", .localizedName = "待重试"},
        {.id = "New", .localizedName = "新增"},
    };
    std::unordered_map<std::string, skill_editor::PassiveSkillMetadata> cache{
        {"Cached",
         skill_editor::PassiveSkillMetadata{.rank = 3, .addWorldTreePal = false, .category = rare}},
    };

    skill_editor::PassiveSkillClassificationJob job;
    job.start(skills, cache);
    CHECK(job.status().total == 3);
    CHECK(job.status().completed == 1);
    CHECK(job.next_batch(8) == std::vector<std::string>({"Retry", "New"}));

    const std::vector<skill_editor::PassiveSkillMetadataReadResult> firstBatch{
        {.id = "Retry", .metadata = std::nullopt},
        {.id = "New",
         .metadata =
             skill_editor::PassiveSkillMetadata{
                 .rank = 4, .addWorldTreePal = false, .category = premium}},
    };
    CHECK(job.complete_batch(firstBatch, cache));
    CHECK(job.status().ready);
    CHECK(cache.contains("New"));
    CHECK(!cache.contains("Retry"));

    job.start(skills, cache);
    CHECK(job.next_batch(8) == std::vector<std::string>({"Retry"}));
}

void test_passive_classification_job_honors_batch_limit_and_reports_failure() {
    std::vector<skill_editor::SkillOption> skills;
    for (int index = 0; index < 10; ++index) {
        skills.push_back({.id = "Passive_" + std::to_string(index), .localizedName = "技能"});
    }
    std::unordered_map<std::string, skill_editor::PassiveSkillMetadata> cache;

    skill_editor::PassiveSkillClassificationJob job;
    job.start(skills, cache);
    CHECK(job.next_batch(8).size() == 8);

    job.fail("missing Rank property");
    CHECK(!job.active());
    CHECK(!job.status().ready);
    CHECK(job.status().error == "missing Rank property");
}

void test_passive_metadata_merge_keeps_unknown_skills_in_catalog() {
    const std::vector<skill_editor::SkillOption> original{
        {.id = "Known", .localizedName = "已知"},
        {.id = "Unknown", .localizedName = "未知"},
    };
    auto skills = original;
    const std::unordered_map<std::string, skill_editor::PassiveSkillMetadata> cache{
        {"Known",
         skill_editor::PassiveSkillMetadata{.rank = 3,
                                            .addWorldTreePal = false,
                                            .category = skill_editor::PassiveSkillCategory::rare}},
    };

    skill_editor::apply_passive_metadata(skills, cache);
    CHECK(skills.size() == original.size());
    CHECK(skills[0].passiveMetadata.has_value());
    CHECK(!skills[1].passiveMetadata.has_value());
}

void test_catalog_fallback_preserves_previous_passive_classification() {
    const skill_editor::SkillCatalogSnapshot previous{
        .passive =
            {
                .skills = {{.id = "Old", .localizedName = "旧技能"}},
                .ready = true,
            },
        .passiveClassification =
            {
                .completed = 1,
                .total = 1,
                .ready = true,
            },
    };
    const skill_editor::SkillCatalogSnapshot failedRefresh{
        .passive = {.error = "refresh failed"},
    };

    const auto merged = skill_editor::with_catalog_fallback(previous, failedRefresh);
    CHECK(merged.passive.skills.front().id == "Old");
    CHECK(merged.passiveClassification.ready);
    CHECK(merged.passive.error == "refresh failed");
}

void test_classification_failure_reuses_only_an_established_category_snapshot() {
    const skill_editor::PassiveSkillClassificationStatus failed{
        .completed = 5,
        .total = 10,
        .error = "missing Rank property",
        .ready = false,
    };

    const auto initialFailure = skill_editor::with_passive_classification_fallback(failed, false);
    CHECK(!initialFailure.ready);
    CHECK(initialFailure.error == "missing Rank property");

    const auto refreshFailure = skill_editor::with_passive_classification_fallback(failed, true);
    CHECK(refreshFailure.ready);
    CHECK(refreshFailure.error == "missing Rank property");
}

void test_passive_classification_cancel_does_not_mutate_success_cache() {
    const std::vector<skill_editor::SkillOption> skills{
        {.id = "Known", .localizedName = "已知"},
        {.id = "Pending", .localizedName = "待处理"},
    };
    std::unordered_map<std::string, skill_editor::PassiveSkillMetadata> cache{
        {"Known",
         skill_editor::PassiveSkillMetadata{.rank = 3,
                                            .addWorldTreePal = false,
                                            .category = skill_editor::PassiveSkillCategory::rare}},
    };

    skill_editor::PassiveSkillClassificationJob job;
    job.start(skills, cache);
    job.cancel();

    CHECK(!job.active());
    CHECK(cache.contains("Known"));
}

void test_passive_skill_presets_have_expected_palworld_1_0_ids() {
    const auto presets = skill_editor::passive_skill_presets();
    CHECK(presets.size() == 2);
    CHECK(presets[0].displayName == "工作毕业1");
    CHECK((presets[0].passiveIds == std::array<std::string_view, 4>{"WorldTree_CraftSpeed",
                                                                    "CraftSpeed_up3", "Vampire",
                                                                    "CraftSpeed_up2"}));
    CHECK(presets[1].displayName == "工作毕业2");
    CHECK((presets[1].passiveIds ==
           std::array<std::string_view, 4>{"WorldTree_CraftSpeed", "CraftSpeed_up3",
                                           "CraftSpeed_up2", "PAL_CorporateSlave"}));
}

void test_passive_skill_preset_definitions_are_valid() {
    CHECK(skill_editor::passive_skill_presets_are_valid());
}

void test_passive_skill_preset_builds_one_world_bound_request() {
    skill_editor::SkillEditQueue queue;
    const auto& preset = skill_editor::passive_skill_presets().front();

    queue.push(skill_editor::make_passive_preset_request(preset, 17, 23));

    CHECK(queue.size() == 1);
    const auto request = queue.try_pop();
    CHECK(request.has_value());
    CHECK(request->targetGeneration == 17);
    CHECK(request->worldGeneration == 23);
    CHECK(request->kind == skill_editor::SkillKind::passive);
    CHECK(request->operation == skill_editor::SkillEditOperation::replaceAllPassives);
    CHECK((request->desiredPassiveIds == std::vector<std::string>{"WorldTree_CraftSpeed",
                                                                  "CraftSpeed_up3", "Vampire",
                                                                  "CraftSpeed_up2"}));
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

void test_internal_active_skill_filter() {
    CHECK(skill_editor::is_internal_active_skill_id("Human_Punch"));
    CHECK(skill_editor::is_internal_active_skill_id("Unique_MoonQueen_GYM_Act"));
    CHECK(skill_editor::is_internal_active_skill_id("RaidCutter"));
    CHECK(skill_editor::is_internal_active_skill_id("Unique_LilyQueen_LilyHealing_Boss"));
    CHECK(!skill_editor::is_internal_active_skill_id("SelfDestruct"));
    CHECK(!skill_editor::is_internal_active_skill_id("MudShot"));
    CHECK(!skill_editor::is_internal_active_skill_id("Unique_Boar_Tackle"));

    constexpr std::array definitions{
        skill_editor::ActiveSkillDefinition{.value = 1, .id = "Human_Punch"},
        skill_editor::ActiveSkillDefinition{.value = 15, .id = "Unique_Boar_Tackle"},
        skill_editor::ActiveSkillDefinition{.value = 124, .id = "MudShot"},
    };
    const auto options = skill_editor::make_active_skill_options(
        definitions, [](const auto&) { return std::string{}; });
    CHECK(options.size() == 2);
    CHECK(options[0].id == "Unique_Boar_Tackle");
    CHECK(options[1].id == "MudShot");
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

    CHECK(scheduler.should_refresh(false, false, start, [] { return true; }));
    CHECK(!scheduler.should_refresh(false, false, start + 1s, [] { return true; }));
    CHECK(scheduler.should_refresh(false, false, start + 2s, [] { return true; }));
    CHECK(!scheduler.should_refresh(false, true, start + 4s, [] { return true; }));
}

void test_catalog_refresh_scheduler_honors_manual_refresh_immediately() {
    using namespace std::chrono_literals;
    skill_editor::SkillCatalogRefreshScheduler scheduler{2s};
    const auto start = skill_editor::SkillCatalogRefreshScheduler::time_point{};

    CHECK(scheduler.should_refresh(false, false, start, [] { return true; }));
    CHECK(scheduler.should_refresh(true, false, start + 100ms, [] { return true; }));
    CHECK(scheduler.should_refresh(true, true, start + 200ms, [] { return true; }));
}

void test_catalog_refresh_scheduler_defers_unsafe_runtime_queries() {
    using namespace std::chrono_literals;
    skill_editor::SkillCatalogRefreshScheduler scheduler{2s};
    const auto start = skill_editor::SkillCatalogRefreshScheduler::time_point{};
    auto readinessChecks = 0;

    CHECK(!scheduler.should_refresh(false, false, start, [&readinessChecks] {
        ++readinessChecks;
        return false;
    }));
    CHECK(readinessChecks == 1);
    CHECK(!scheduler.should_refresh(false, false, start + 1s, [&readinessChecks] {
        ++readinessChecks;
        return true;
    }));
    CHECK(readinessChecks == 1);
    CHECK(scheduler.should_refresh(false, false, start + 2s, [&readinessChecks] {
        ++readinessChecks;
        return true;
    }));
    CHECK(readinessChecks == 2);
}

void test_catalog_refresh_scheduler_never_bypasses_runtime_gate() {
    using namespace std::chrono_literals;
    skill_editor::SkillCatalogRefreshScheduler scheduler{2s};
    const auto start = skill_editor::SkillCatalogRefreshScheduler::time_point{};
    auto readinessChecks = 0;

    CHECK(!scheduler.should_refresh(true, false, start, [&readinessChecks] {
        ++readinessChecks;
        return false;
    }));
    CHECK(readinessChecks == 1);
    CHECK(scheduler.should_refresh(true, false, start + 1ms, [&readinessChecks] {
        ++readinessChecks;
        return true;
    }));
    CHECK(readinessChecks == 2);
    CHECK(!scheduler.should_refresh(false, true, start + 2s, [&readinessChecks] {
        ++readinessChecks;
        return true;
    }));
    CHECK(readinessChecks == 2);
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

void test_item_catalog_scan_scheduler_is_bounded_and_world_scoped() {
    item_catalog::ItemCatalogScanScheduler scheduler;
    scheduler.begin_world(7);
    CHECK(scheduler.advance(0.0F, 7, true));
    CHECK(scheduler.complete(7, false));
    CHECK(!scheduler.advance(1.0F, 7, true));
    CHECK(scheduler.advance(1.0F, 7, true));
    CHECK(scheduler.complete(7, true));
    CHECK(scheduler.authoritative_catalog_ready());
    CHECK(!scheduler.advance(10.0F, 7, true));

    scheduler.begin_world(8);
    CHECK(!scheduler.advance(10.0F, 7, true));
    CHECK(scheduler.advance(0.0F, 8, true));
    CHECK(scheduler.complete(8, false));
    for (std::uint8_t attempt{1}; attempt < scheduler.maximumAttempts; ++attempt) {
        CHECK(scheduler.advance(scheduler.retryDelaySeconds, 8, true));
        CHECK(scheduler.complete(8, false));
    }
    CHECK(!scheduler.advance(60.0F, 8, true));

    scheduler.request(8);
    CHECK(scheduler.advance(0.0F, 8, true));
    scheduler.cancel();
    CHECK(!scheduler.advance(60.0F, 8, true));
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

auto passive_set_request(std::vector<std::string> ids) -> skill_editor::SkillEditRequest {
    return {
        .target = 0x1234,
        .kind = skill_editor::SkillKind::passive,
        .operation = skill_editor::SkillEditOperation::replaceAllPassives,
        .desiredPassiveIds = std::move(ids),
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

void test_passive_set_rejects_invalid_definitions_before_writing() {
    const std::array invalidSets{
        std::vector<std::string>{},
        std::vector<std::string>{"A", "B", "C"},
        std::vector<std::string>{"A", "B", "C", "C"},
        std::vector<std::string>{"A", "B", "C", ""},
    };
    for (const auto& ids : invalidSets) {
        FakeSkillGateway gateway;
        gateway.state.passiveIds = {"O1", "O2", "O3", "O4"};
        const auto result = skill_editor::execute_skill_edit(gateway, passive_set_request(ids));
        CHECK(result.status == skill_editor::SkillEditStatus::rejected);
        CHECK(gateway.calls == std::vector<std::string>({"read"}));
    }
}

void test_matching_passive_set_is_zero_write() {
    FakeSkillGateway gateway;
    gateway.state.passiveIds = {"D", "C", "B", "A"};

    const auto result =
        skill_editor::execute_skill_edit(gateway, passive_set_request({"A", "B", "C", "D"}));

    CHECK(result.status == skill_editor::SkillEditStatus::succeeded);
    CHECK(gateway.calls == std::vector<std::string>({"read"}));
}

void test_passive_set_replaces_a_completely_different_set() {
    FakeSkillGateway gateway;
    gateway.state.passiveIds = {"Old1", "Old2", "Old3", "Old4"};

    const auto result = skill_editor::execute_skill_edit(
        gateway, passive_set_request({"New1", "New2", "New3", "New4"}));

    CHECK(result.status == skill_editor::SkillEditStatus::succeeded);
    CHECK(gateway.calls == std::vector<std::string>({"read", "remove:Old1", "remove:Old2",
                                                     "remove:Old3", "remove:Old4", "add:New1",
                                                     "add:New2", "add:New3", "add:New4", "read"}));
    CHECK(skill_editor::detail::same_passives(
        result.state.passiveIds, std::vector<std::string>{"New1", "New2", "New3", "New4"}));
}

void test_passive_set_uses_only_required_difference_writes() {
    FakeSkillGateway gateway;
    gateway.state.passiveIds = {"A", "B", "Old1", "Old2"};

    const auto result =
        skill_editor::execute_skill_edit(gateway, passive_set_request({"A", "B", "New1", "New2"}));

    CHECK(result.status == skill_editor::SkillEditStatus::succeeded);
    CHECK(gateway.calls == std::vector<std::string>({"read", "remove:Old1", "remove:Old2",
                                                     "add:New1", "add:New2", "read"}));
    CHECK(skill_editor::detail::same_passives(result.state.passiveIds,
                                              std::vector<std::string>{"A", "B", "New1", "New2"}));
}

void test_passive_set_rolls_back_after_partial_failure() {
    FakeSkillGateway gateway;
    gateway.state.passiveIds = {"A", "B", "C", "D"};
    gateway.addOutcomes = {true, false, true, true};

    const auto result =
        skill_editor::execute_skill_edit(gateway, passive_set_request({"A", "B", "X", "Y"}));

    CHECK(result.status == skill_editor::SkillEditStatus::rolledBack);
    CHECK(gateway.calls ==
          std::vector<std::string>({"read", "remove:C", "remove:D", "add:X", "add:Y", "read",
                                    "remove:X", "add:C", "add:D", "read"}));
    CHECK(skill_editor::detail::same_passives(result.state.passiveIds,
                                              std::vector<std::string>{"A", "B", "C", "D"}));
}

void test_passive_set_reports_rollback_failure() {
    FakeSkillGateway gateway;
    gateway.state.passiveIds = {"A", "B", "C", "D"};
    gateway.addOutcomes = {true, false, false, true};

    const auto result =
        skill_editor::execute_skill_edit(gateway, passive_set_request({"A", "B", "X", "Y"}));

    CHECK(result.status == skill_editor::SkillEditStatus::rollbackFailed);
    CHECK(gateway.calls ==
          std::vector<std::string>({"read", "remove:C", "remove:D", "add:X", "add:Y", "read",
                                    "remove:X", "add:C", "add:D", "read"}));
    CHECK(!skill_editor::detail::same_passives(result.state.passiveIds,
                                               std::vector<std::string>{"A", "B", "C", "D"}));
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

    const auto originalActiveSkills = gateway.state.activeSkills;
    result = skill_editor::execute_skill_edit(
        gateway, active_request(skill_editor::SkillEditOperation::replaceAllPassives, 0));
    CHECK(result.status == skill_editor::SkillEditStatus::rejected);
    CHECK(result.state.activeSkills == originalActiveSkills);
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

void test_pal_resolution_decision_has_zero_idle_work_after_selection() {
    CHECK(skill_editor::decide_pal_resolution(false, false) ==
          skill_editor::PalResolutionTrigger::none);
    CHECK(skill_editor::decide_pal_resolution(true, false) ==
          skill_editor::PalResolutionTrigger::selectionRequest);
    CHECK(skill_editor::decide_pal_resolution(false, false) ==
          skill_editor::PalResolutionTrigger::none);
    CHECK(skill_editor::decide_pal_resolution(false, false) ==
          skill_editor::PalResolutionTrigger::none);
}

void test_pal_resolution_decision_runs_edits_immediately() {
    CHECK(skill_editor::decide_pal_resolution(false, true) ==
          skill_editor::PalResolutionTrigger::editRequest);
}

void test_pal_resolution_decision_prioritizes_selection() {
    CHECK(skill_editor::decide_pal_resolution(true, true) ==
          skill_editor::PalResolutionTrigger::selectionRequest);
}

void test_target_resolution_snapshot_equality_tracks_observable_changes() {
    const skill_editor::TargetResolutionSnapshot first{
        .resolved = true,
        .observation = {.identity = identity(10), .name = "Boar"},
        .status = skill_editor::SelectedTargetResolutionStatus::success,
        .holderCandidateCount = 1,
        .localHolderCandidateCount = 1,
        .holderCandidateClasses = L"PalOtomoHolderComponent",
    };
    auto same = first;
    CHECK(first == same);
    same.observation.identity = identity(11);
    CHECK(!(first == same));
}

void test_target_resolution_state_marks_only_real_changes() {
    skill_editor::TargetResolutionState state;
    const skill_editor::TargetResolutionSnapshot first{
        .resolved = true,
        .observation = {.identity = identity(10), .name = "Boar"},
        .status = skill_editor::SelectedTargetResolutionStatus::success,
        .holderCandidateCount = 1,
        .localHolderCandidateCount = 1,
        .holderCandidateClasses = L"PalOtomoHolderComponent",
    };

    CHECK(state.update(first));
    CHECK(!state.update(first));
    auto changed = first;
    changed.status = skill_editor::SelectedTargetResolutionStatus::parameterUnavailable;
    CHECK(state.update(changed));
    CHECK(state.current() == changed);
    state.reset();
    CHECK(state.current() == skill_editor::TargetResolutionSnapshot{});
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

void test_world_bound_selection_request_expires_across_transition() {
    skill_editor::WorldSessionState session;
    const skill_editor::WorldBoundRequest oldRequest{
        .worldGeneration = session.generation(),
    };
    CHECK(skill_editor::request_can_run(oldRequest, session));

    session.begin_transition();
    session.finish_transition();
    CHECK(!skill_editor::request_can_run(oldRequest, session));

    const skill_editor::WorldBoundRequest currentRequest{
        .worldGeneration = session.generation(),
    };
    CHECK(skill_editor::request_can_run(currentRequest, session));
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
        {.targetGeneration = state.generation() + 1, .worldGeneration = session.generation()},
        state, observed, 0x2000, session, apply);
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

void test_pal_stat_request_requires_the_locked_guid() {
    skill_editor::SelectedTargetState state;
    skill_editor::WorldSessionState session;
    const skill_editor::SelectedTargetObservation locked{
        .identity = identity(10),
        .name = "Boar",
    };
    const skill_editor::SelectedTargetObservation other{
        .identity = identity(11),
        .name = "Sheep",
    };
    CHECK(state.confirm(locked));
    CHECK(session.confirm_target());

    const pal_stats::PalStatEditRequest request{
        .values = {.level = 20},
        .targetGeneration = state.generation(),
        .worldGeneration = session.generation(),
    };
    CHECK(skill_editor::bound_target_request_is_current(request, state, locked, 0x2000, session));
    CHECK(!skill_editor::bound_target_request_is_current(request, state, other, 0x2000, session));
    CHECK(!skill_editor::bound_target_request_is_current(request, state, locked, 0, session));
}

void test_pal_stat_clamp_respects_policy_bounds() {
    using namespace pal_stats;
    // 等级 1–80
    CHECK(clamp_level(0) == 1);
    CHECK(clamp_level(1) == 1);
    CHECK(clamp_level(50) == 50);
    CHECK(clamp_level(80) == 80);
    CHECK(clamp_level(81) == 80);
    CHECK(clamp_level(255) == 80);
    // 普通个体值 0–100，拒绝生成超出游戏正常范围的存档数据。
    CHECK(clamp_talent(-5) == 0);
    CHECK(clamp_talent(0) == 0);
    CHECK(clamp_talent(100) == 100);
    CHECK(clamp_talent(255) == 100);
    CHECK(clamp_talent(256) == 100);
    // 亲密度 rank 0–10
    CHECK(clamp_friendship_rank(-1) == 0);
    CHECK(clamp_friendship_rank(0) == 0);
    CHECK(clamp_friendship_rank(10) == 10);
    CHECK(clamp_friendship_rank(11) == 10);
}

void test_pal_stat_values_detects_any_change() {
    using namespace pal_stats;
    CHECK(!has_any_change(PalStatValues{}));
    CHECK(has_any_change(PalStatValues{.level = 1}));
    CHECK(has_any_change(PalStatValues{.talentHp = 0}));
    CHECK(has_any_change(PalStatValues{.friendshipRank = 10}));
}

void test_pal_stat_verification_checks_only_requested_fields() {
    const pal_stats::PalStatSnapshot actual{
        .level = 45,
        .talentHp = 90,
        .talentShot = 82,
        .talentDefense = 63,
        .friendshipRank = 4,
        .friendshipPoint = 1200,
        .readable = true,
    };

    CHECK(pal_stats::verify_stat_edit({.talentHp = 90}, actual));
    CHECK(pal_stats::verify_stat_edit({.level = 45, .friendshipRank = 4}, actual));
    CHECK(!pal_stats::verify_stat_edit({.talentHp = 89}, actual));

    auto unreadable = actual;
    unreadable.readable = false;
    CHECK(!pal_stats::verify_stat_edit({.talentHp = 90}, unreadable));
}

void test_pal_stat_draft_starts_from_snapshot_and_emits_only_changes() {
    pal_stats::PalStatEditDraft draft;
    draft.synchronize({.level = 45,
                       .talentHp = 71,
                       .talentShot = 82,
                       .talentDefense = 63,
                       .friendshipRank = 4,
                       .friendshipPoint = 1200,
                       .readable = true},
                      7);

    CHECK(draft.values().level == 45);
    CHECK(draft.values().talentHp == 71);
    CHECK(!draft.make_request(11).has_value());

    draft.values().talentHp = 90;
    const auto request = draft.make_request(11);
    CHECK(request.has_value());
    CHECK(!request->values.level.has_value());
    CHECK(request->values.talentHp == 90);
    CHECK(!request->values.talentShot.has_value());
    CHECK(!request->values.talentDefense.has_value());
    CHECK(!request->values.friendshipRank.has_value());
    CHECK(request->targetGeneration == 7);
    CHECK(request->worldGeneration == 11);
}

void test_pal_stat_request_slot_keeps_only_latest_request() {
    pal_stats::PalStatEditRequestSlot slot;
    slot.submit({.values = {.level = 10}, .targetGeneration = 1, .worldGeneration = 2});
    slot.submit({.values = {.level = 20}, .targetGeneration = 1, .worldGeneration = 2});

    CHECK(slot.has_pending());
    const auto request = slot.consume();
    CHECK(request.has_value());
    CHECK(request->values.level == 20);
    CHECK(!slot.has_pending());
}

auto main() -> int {
    test_skill_catalog_search_and_labels();
    test_skill_catalog_filter_and_deduplicate();
    test_passive_skill_categories_follow_runtime_metadata();
    test_passive_filter_combines_category_exclusion_and_search();
    test_passive_filter_views_preserve_catalog_objects_and_order();
    test_passive_picker_category_change_clears_only_selection();
    test_passive_classification_job_reuses_success_cache_and_retries_unknowns();
    test_passive_classification_job_honors_batch_limit_and_reports_failure();
    test_passive_metadata_merge_keeps_unknown_skills_in_catalog();
    test_catalog_fallback_preserves_previous_passive_classification();
    test_classification_failure_reuses_only_an_established_category_snapshot();
    test_passive_classification_cancel_does_not_mutate_success_cache();
    test_passive_skill_presets_have_expected_palworld_1_0_ids();
    test_passive_skill_preset_definitions_are_valid();
    test_passive_skill_preset_builds_one_world_bound_request();
    test_active_skill_definitions_are_unique_and_known_values_match();
    test_internal_active_skill_filter();
    test_active_skill_options_use_runtime_localization_with_raw_id_fallback();
    test_skill_catalog_refresh_merges_sections_independently();
    test_skill_catalog_first_partial_load_keeps_available_section();
    test_partial_catalog_is_not_ready_for_editing();
    test_catalog_fallback_preserves_established_runtime_readiness();
    test_catalog_refresh_scheduler_throttles_automatic_retries();
    test_catalog_refresh_scheduler_honors_manual_refresh_immediately();
    test_catalog_refresh_scheduler_defers_unsafe_runtime_queries();
    test_catalog_refresh_scheduler_never_bypasses_runtime_gate();
    test_item_catalog_labels_and_search();
    test_item_catalog_scan_scheduler_is_bounded_and_world_scoped();
    test_item_catalog_deduplicates_indexes_and_sorts();
    test_passive_edits_validate_target_and_limits();
    test_passive_add_remove_and_replace_reread_state();
    test_passive_replace_rolls_back_on_failure();
    test_passive_set_rejects_invalid_definitions_before_writing();
    test_matching_passive_set_is_zero_write();
    test_passive_set_replaces_a_completely_different_set();
    test_passive_set_uses_only_required_difference_writes();
    test_passive_set_rolls_back_after_partial_failure();
    test_passive_set_reports_rollback_failure();
    test_active_edits_validate_three_compact_slots();
    test_active_add_replace_and_remove_preserve_order();
    test_active_edit_rolls_back_complete_original_sequence();
    test_skill_edit_queue_is_fifo();
    test_skill_edit_queue_can_discard_all_pending_requests();
    test_pal_resolution_decision_has_zero_idle_work_after_selection();
    test_pal_resolution_decision_runs_edits_immediately();
    test_pal_resolution_decision_prioritizes_selection();
    test_target_resolution_snapshot_equality_tracks_observable_changes();
    test_target_resolution_state_marks_only_real_changes();
    test_target_requires_explicit_confirmation();
    test_world_session_transition_requires_reconfirmation();
    test_world_session_cannot_confirm_during_transition();
    test_world_bound_selection_request_expires_across_transition();
    test_observations_do_not_replace_or_clear_explicit_target();
    test_resolution_status_has_actionable_message();
    test_unique_local_candidate_is_selected();
    test_local_candidate_selection_reports_each_unavailable_stage();
    test_multiple_local_candidates_are_rejected();
    test_stale_generation_never_reaches_apply_callback();
    test_pal_stat_request_requires_the_locked_guid();
    test_pal_stat_clamp_respects_policy_bounds();
    test_pal_stat_values_detects_any_change();
    test_pal_stat_verification_checks_only_requested_fields();
    test_pal_stat_draft_starts_from_snapshot_and_emits_only_changes();
    test_pal_stat_request_slot_keeps_only_latest_request();
    return failures == 0 ? 0 : 1;
}
