#include <array>
#include <filesystem>
#include <iostream>
#include <system_error>
#include <vector>

#include <base_resource_sharing/hook_manifest.hpp>
#include <base_resource_sharing/resource_pool.hpp>
#include <base_resource_sharing/resource_session.hpp>
#include <base_resource_sharing/settings.hpp>

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

void test_settings_default_off_and_round_trip() {
    using namespace base_resource_sharing;

    const auto missing = parse_settings("");
    CHECK(!missing.settings.enabled);
    CHECK(!missing.error.empty());

    const auto enabled = parse_settings(
        "[BaseResourceSharing]\n"
        "Enabled=true\n");
    CHECK(enabled.settings.enabled);
    CHECK(enabled.error.empty());
    CHECK(serialize_settings(enabled.settings) == "[BaseResourceSharing]\nEnabled=true\n");

    const auto invalid = parse_settings(
        "[BaseResourceSharing]\n"
        "Enabled=maybe\n");
    CHECK(!invalid.settings.enabled);
    CHECK(!invalid.error.empty());
}

void test_settings_file_round_trip() {
    using namespace base_resource_sharing;

    const auto root =
        std::filesystem::temp_directory_path() / "PalworldEditorBaseResourceSharingTests";
    const auto path = root / "config.ini";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);

    const auto missing = load_settings(path);
    CHECK(!missing.settings.enabled);
    CHECK(!missing.error.empty());
    CHECK(!std::filesystem::exists(path));

    CHECK(save_settings(path, Settings{.enabled = true}).empty());
    const auto loaded = load_settings(path);
    CHECK(loaded.settings.enabled);
    CHECK(loaded.error.empty());

    std::filesystem::remove_all(root, ignored);
}

void test_resource_pool_filters_deduplicates_and_orders() {
    using namespace base_resource_sharing;

    const GuidKey guild{{1, 0, 0, 0}};
    const GuidKey otherGuild{{2, 0, 0, 0}};
    const GuidKey baseA{{10, 0, 0, 0}};
    const GuidKey baseB{{20, 0, 0, 0}};
    const std::vector<ContainerDescriptor> containers{
        {.baseId = baseB,
         .groupId = guild,
         .containerId = {{202, 0, 0, 0}},
         .kind = ContainerKind::normal},
        {.baseId = baseA,
         .groupId = guild,
         .containerId = {{101, 0, 0, 0}},
         .kind = ContainerKind::normal,
         .currentBase = true},
        {.baseId = baseA,
         .groupId = guild,
         .containerId = {{101, 0, 0, 0}},
         .kind = ContainerKind::normal,
         .currentBase = true},
        {.baseId = baseA,
         .groupId = guild,
         .containerId = {{102, 0, 0, 0}},
         .kind = ContainerKind::food,
         .currentBase = true},
        {.baseId = baseB,
         .groupId = otherGuild,
         .containerId = {{203, 0, 0, 0}},
         .kind = ContainerKind::normal},
    };

    const auto plan = make_resource_union_plan(containers, guild);
    CHECK(plan.error.empty());
    CHECK(plan.baseCount == 2);
    CHECK(plan.ordered.size() == 2);
    const GuidKey expectedFirst{{101, 0, 0, 0}};
    const GuidKey expectedSecond{{202, 0, 0, 0}};
    CHECK(plan.ordered[0].containerId == expectedFirst);
    CHECK(plan.ordered[1].containerId == expectedSecond);

    CHECK(!make_resource_union_plan(containers, {}).error.empty());
    CHECK(!make_resource_union_plan({}, guild).error.empty());
}

void test_runtime_state_fails_closed_across_worlds() {
    using namespace base_resource_sharing;

    RuntimeState state;
    state.set_preference(true);
    state.finish_world_transition(7);
    state.set_capability(ResourceOperation::crafting,
                         CapabilityState{.previewReady = true, .consumeReady = true});

    CHECK(state.can_extend(ResourceOperation::crafting, 7));
    CHECK(!state.can_extend(ResourceOperation::building, 7));

    state.begin_world_transition(8);
    CHECK(!state.can_extend(ResourceOperation::crafting, 7));
    CHECK(!state.can_extend(ResourceOperation::crafting, 8));

    state.finish_world_transition(8);
    CHECK(!state.can_extend(ResourceOperation::crafting, 8));
}

void test_hook_capabilities_require_preview_and_consume_paths() {
    using namespace base_resource_sharing;

    auto resolved = all_hook_resolutions(false);
    mark_resolved(resolved, "/Script/Pal.PalUIProductSettingModel:CalcMaxProductableNum");
    mark_resolved(resolved, "/Script/Pal.PalUIConvertItemModel:StartProduction");
    auto capabilities = evaluate_capabilities(resolved);
    CHECK(capabilities[operation_index(ResourceOperation::crafting)].available());
    CHECK(!capabilities[operation_index(ResourceOperation::building)].available());
    CHECK(!capabilities[operation_index(ResourceOperation::repair)].available());

    mark_resolved(resolved, "/Script/Pal.PalBuilderComponent:IsExistsMaterialForBuildObject");
    mark_resolved(resolved, "/Script/Pal.PalNetworkPlayerComponent:RequestBuild_ToServer");
    capabilities = evaluate_capabilities(resolved);
    CHECK(capabilities[operation_index(ResourceOperation::building)].available());
    CHECK(capabilities[operation_index(ResourceOperation::repair)].error ==
          "Palworld 1.0.1 尚未验证安全的修理材料检查与扣除入口。");
}

void test_discovery_rejects_partially_resolved_container_sets() {
    using namespace base_resource_sharing;

    const GuidKey guild{{1, 0, 0, 0}};
    const std::vector<ContainerDescriptor> registered{
        {.baseId = {{10, 0, 0, 0}},
         .groupId = guild,
         .containerId = {{101, 0, 0, 0}},
         .kind = ContainerKind::normal},
        {.baseId = {{20, 0, 0, 0}},
         .groupId = guild,
         .containerId = {{201, 0, 0, 0}},
         .kind = ContainerKind::normal},
    };
    const std::array firstOnly{registered[0].containerId};
    const std::array both{registered[0].containerId, registered[1].containerId};

    CHECK(validate_live_container_resolution(registered, firstOnly).error ==
          "仅解析到 1/2 个已登记据点资源容器。");
    CHECK(validate_live_container_resolution(registered, both).error.empty());
}

void test_union_restoration_accepts_only_the_recorded_tail() {
    using namespace base_resource_sharing;

    const GuidKey a{{1, 0, 0, 0}};
    const GuidKey b{{2, 0, 0, 0}};
    const GuidKey c{{3, 0, 0, 0}};
    const std::array original{a};
    const std::array correctCurrent{a, b, c};
    const std::array reorderedCurrent{a, c, b};
    const std::array missingPrefix{b, c};
    const std::array appended{b, c};

    CHECK(verify_restoration_sequence(original, correctCurrent, appended));
    CHECK(!verify_restoration_sequence(original, reorderedCurrent, appended));
    CHECK(!verify_restoration_sequence(original, missingPrefix, appended));

    const std::array partialPlan{a, c};
    const std::array globalPlan{a, b, c};
    CHECK(missing_union_tail(original, globalPlan) == std::vector<GuidKey>({b, c}));
    CHECK(missing_union_tail(partialPlan, globalPlan) == std::vector<GuidKey>({b}));
}

void test_recorded_injection_removal_preserves_runtime_native_changes() {
    using namespace base_resource_sharing;

    const GuidKey a{{1, 0, 0, 0}};
    const GuidKey b{{2, 0, 0, 0}};
    const GuidKey c{{3, 0, 0, 0}};
    const GuidKey d{{4, 0, 0, 0}};
    const std::array original{a};
    const std::array current{a, b, c, d, b};
    const std::array injected{b, c};

    const auto plan = remove_recorded_injections(current, original, injected);
    CHECK(plan.complete);
    CHECK(plan.kept == std::vector<GuidKey>({a, d, b}));

    const std::array missing{a, b};
    CHECK(!remove_recorded_injections(missing, original, injected).complete);
}

void test_request_guards_and_build_window_are_generation_safe() {
    using namespace base_resource_sharing;

    RequestGuard guard;
    CHECK(guard.try_enter(ResourceOperation::crafting, 12));
    CHECK(!guard.try_enter(ResourceOperation::crafting, 12));
    guard.leave(ResourceOperation::crafting, 12);
    CHECK(!guard.active());

    BuildUnionWindow window;
    CHECK(window.open(42));
    CHECK(!window.advance(0.74F, 42));
    CHECK(window.advance(0.01F, 42));
    CHECK(!window.opened());
    CHECK(window.open(43));
    CHECK(window.advance(0.01F, 44));
    CHECK(!window.opened());
}

void test_status_text_reports_partial_support() {
    using namespace base_resource_sharing;

    const BaseResourceSharingStatus status{
        .enabled = true,
        .baseCount = 3,
        .containerCount = 12,
        .craftingAvailable = true,
        .buildingAvailable = true,
        .repairAvailable = false,
        .repairError = "Palworld 1.0.1 尚未验证安全的修理材料检查与扣除入口。",
    };
    const auto text = format_status(status);
    CHECK(text.find("3 个据点") != std::string::npos);
    CHECK(text.find("12 个资源容器") != std::string::npos);
    CHECK(text.find("制作：可用") != std::string::npos);
    CHECK(text.find("建造：可用") != std::string::npos);
    CHECK(text.find("修理：不可用") != std::string::npos);

    const BaseResourceSharingStatus detecting{
        .enabled = true,
        .detectingCapabilities = true,
        .craftingError = "缺少必需接口：CraftPreview",
        .buildingError = "缺少必需接口：BuildConsume",
    };
    const auto detectingText = format_status(detecting);
    CHECK(detectingText.find("CraftPreview") != std::string::npos);
    CHECK(detectingText.find("BuildConsume") != std::string::npos);
}

void test_disabled_resource_sharing_has_no_runtime_work() {
    using namespace base_resource_sharing;

    CHECK(!resource_hooks_required(false, true));
    CHECK(!resource_hooks_required(true, false));
    CHECK(resource_hooks_required(true, true));

    SnapshotDirtyFlag dirty;
    CHECK(dirty.consume());
    CHECK(!dirty.consume());
    dirty.mark();
    CHECK(dirty.consume());
}

void test_preview_cache_expires_at_one_second_and_on_world_change() {
    using namespace base_resource_sharing;

    PreviewCacheGate cache;
    cache.record(7, 10.0);
    CHECK(cache.can_reuse(7, 10.999));
    CHECK(!cache.can_reuse(7, 11.0));
    CHECK(!cache.can_reuse(8, 10.1));
    cache.invalidate();
    CHECK(!cache.can_reuse(7, 10.1));
}

void test_preview_amounts_merge_duplicates_and_preserve_vanilla() {
    using namespace base_resource_sharing;

    const std::array raw{ItemAmount{"Wood", 7}, ItemAmount{"Wood", 5}, ItemAmount{"Stone", 2}};
    const auto aggregated = aggregate_amounts(raw);
    CHECK(aggregated.error.empty());
    CHECK(aggregated.amounts.at("Wood") == 12);

    const std::array requirements{ItemAmount{"Wood", 3}, ItemAmount{"Wood", 2},
                                  ItemAmount{"Stone", 1}};
    CHECK(max_productable_from_shared_counts(1, requirements, aggregated.amounts) == 2);
    CHECK(shared_requirements_available(requirements, aggregated.amounts));

    const std::array invalid{ItemAmount{"Wood", -1}};
    CHECK(!aggregate_amounts(invalid).error.empty());
}

void test_hook_manifest_contains_only_top_level_preview_and_consume_paths() {
    using namespace base_resource_sharing;

    const auto hooks = palworld_1_0_1_hook_manifest();
    CHECK(hooks.size() == 4);
    CHECK(std::ranges::count(hooks, HookRole::preview, &HookSpec::role) == 2);
    CHECK(std::ranges::count(hooks, HookRole::consume, &HookSpec::role) == 2);
}

void test_preview_sources_combine_player_and_base_storage() {
    using namespace base_resource_sharing;

    const std::array player{ItemAmount{"Ingot", 4}};
    const std::array bases{ItemAmount{"Ingot", 6}};
    const auto total = combine_preview_sources(player, bases);
    const std::array recipe{ItemAmount{"Ingot", 10}};
    CHECK(total.error.empty());
    CHECK(max_productable_from_shared_counts(0, recipe, total.amounts) == 1);
}

void test_reconcile_scheduler_coalesces_events_and_uses_bounded_intervals() {
    using namespace base_resource_sharing;

    ReconcileScheduler scheduler;
    scheduler.begin_world(7);
    CHECK(scheduler.advance(0.0F, 7));
    scheduler.complete(true, 7);
    CHECK(!scheduler.advance(7.999F, 7));
    CHECK(scheduler.advance(0.001F, 7));
    scheduler.complete(true, 7);

    scheduler.request_immediate(7);
    scheduler.request_immediate(7);
    CHECK(scheduler.advance(0.0F, 7));
    CHECK(!scheduler.advance(0.0F, 7));
    scheduler.complete(false, 7);
    CHECK(!scheduler.advance(0.999F, 7));
    CHECK(scheduler.advance(0.001F, 7));
    scheduler.complete(true, 7);

    CHECK(!scheduler.advance(8.0F, 8));
}

void test_union_leases_overlap_and_crafting_expires_after_idle() {
    using namespace base_resource_sharing;

    ResourceUnionLeaseState leases;
    leases.begin_world(11);
    CHECK(leases.acquire_building(11));
    CHECK(leases.touch_crafting(11));
    CHECK(leases.desired(11));
    CHECK(!leases.advance(1.5F, 11));
    CHECK(leases.desired(11));
    CHECK(!leases.release_building(12));
    CHECK(leases.release_building(11));
    CHECK(!leases.desired(11));

    CHECK(leases.touch_crafting(11));
    CHECK(!leases.advance(1.499F, 11));
    CHECK(leases.advance(0.001F, 11));
    CHECK(!leases.desired(11));
    CHECK(!leases.touch_crafting(12));
}

auto main() -> int {
    test_settings_default_off_and_round_trip();
    test_settings_file_round_trip();
    test_resource_pool_filters_deduplicates_and_orders();
    test_runtime_state_fails_closed_across_worlds();
    test_hook_capabilities_require_preview_and_consume_paths();
    test_discovery_rejects_partially_resolved_container_sets();
    test_union_restoration_accepts_only_the_recorded_tail();
    test_recorded_injection_removal_preserves_runtime_native_changes();
    test_request_guards_and_build_window_are_generation_safe();
    test_status_text_reports_partial_support();
    test_disabled_resource_sharing_has_no_runtime_work();
    test_preview_cache_expires_at_one_second_and_on_world_change();
    test_preview_amounts_merge_duplicates_and_preserve_vanilla();
    test_hook_manifest_contains_only_top_level_preview_and_consume_paths();
    test_preview_sources_combine_player_and_base_storage();
    test_reconcile_scheduler_coalesces_events_and_uses_bounded_intervals();
    test_union_leases_overlap_and_crafting_expires_after_idle();
    return failures == 0 ? 0 : 1;
}
